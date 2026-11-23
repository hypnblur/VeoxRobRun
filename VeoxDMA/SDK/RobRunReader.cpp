#include "pch.h"
#include "RobRunReader.h"

extern void RequestReattach();
#include "RobRunMemory.h"
#include "Il2Cpp.h"
#include "Memory.h"
#include "CameraFinder.h"
#include "CollisionBVH.h"
#include "ConfigInstance.h"
#include "DebugLog.h"
#include "Overlay.h"
#include <atomic>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <intrin.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace RobRunReader {

static std::uintptr_t gaBase = 0;
static std::uintptr_t upBase = 0;

static std::uintptr_t cachedGame = 0;
static std::atomic<std::uintptr_t> cachedBattleModel{ 0 };
static std::atomic<std::uintptr_t> cachedBattleEntities{ 0 };
static std::uintptr_t cachedCameraModel = 0;
static std::atomic<std::uintptr_t> cachedLocalCharacter{ 0 };
static std::uintptr_t cachedLookDirectionModel = 0;
static std::uintptr_t cachedLookDirDirect = 0;
static std::uintptr_t cachedPlayerRotationInput = 0;
static std::uintptr_t cachedUpdater = 0;

// Scene/session identity. Every root change (level transition, new match)
// bumps the generation so memory aim can stop writing until the new objects
// have been re-resolved.
static int32_t localEntityId = -1;
static std::atomic<std::uint64_t> sessionGeneration{ 0 };
static std::uint64_t sessionChangedMs = 0;
static std::uintptr_t lastBattleModel = 0;
static std::uintptr_t lastBattleEntities = 0;
static std::uintptr_t lastCameraModel = 0;
static std::uintptr_t lastLocalCharacter = 0;
static std::uintptr_t lastUpdater = 0;
static std::int32_t lastLocalEntityId = -1;
static bool lastSessionValid = false;

// Local party membership, resolved from the battle party models. The aim
// teammate filter matches these ids instead of comparing roles, so players
// outside the party stay targetable even when they share the local role.
struct PartyMemberIds {
    std::vector<std::int32_t> entityIds;
    std::vector<std::uint64_t> publicIds;
    bool valid = false;
};
static PartyMemberIds g_party;
static std::uint64_t g_partyStamp = 0;

static std::unordered_map<std::int32_t, std::uintptr_t> cachedSlots;
static std::uint64_t slotRefreshStamp = 0;
// Worker-owned view cache. Threading contract: only the read worker writes
// cachedViews; the render thread consumes the published Snapshot instead of
// touching this map, so no lock is required as long as that stays true.
static std::unordered_map<std::int32_t, std::uintptr_t> cachedViews;
static std::unordered_map<std::int32_t, std::uintptr_t> cachedViewOwner;
static std::unordered_map<std::int32_t, int> viewOwnerMisses;
static bool viewsRefreshed = false;
static std::uint64_t viewRefreshStamp = 0;
static std::uint64_t cameraResolveStamp = 0;
static std::uint64_t localChainStamp = 0;
static std::uint64_t fullResolveStamp = 0;
static std::string cachedLocationId;
static std::mutex locationMutex;
static std::uint64_t locationStamp = 0;

// Caches for immutable entity identity, role and weapon descriptions
struct SlotIdentityCache {
    std::wstring nickname;
    std::wstring clanTag;
    std::uint64_t publicId = 0;
    bool valid = false;
};
static std::unordered_map<std::uintptr_t, SlotIdentityCache> g_slotIdentityCache;
static std::unordered_map<std::uint64_t, std::string> g_publicIdToNick;

struct RoleCacheEntry {
    int roleId = 0;
    std::string roleName;
    bool resolved = false;
};
static std::unordered_map<std::uintptr_t, RoleCacheEntry> g_roleCache;

struct WeaponDescCache {
    std::wstring name;
    int index = -1;
    bool valid = false;
};
static std::unordered_map<std::uintptr_t, WeaponDescCache> g_weaponDescCache;

static std::atomic<bool> g_cameraInvalidated{ false };

// Lightweight render-thread visibility feedback (written lock-free from
// PlayerEsp::DrawPlayers, read by the worker and world threads to time
// manual VMM Mem/TLB cache refreshes when nobody is in sight or nearby).
static std::atomic<bool> g_renderAnyOnScreen{ false };
static std::atomic<float> g_renderNearestDist{ 9999.f };
static std::atomic<std::uint64_t> g_renderReportMs{ 0 };
static std::atomic<std::uint64_t> g_lastCacheRefreshMs{ 0 };

void ReportRenderState(bool anyOnScreen, float nearestOnScreenDist)
{
    g_renderAnyOnScreen.store(anyOnScreen, std::memory_order_relaxed);
    g_renderNearestDist.store(nearestOnScreenDist, std::memory_order_relaxed);
    g_renderReportMs.store(GetTickCount64(), std::memory_order_relaxed);
}

static bool MaybeRefreshDmaCaches(const char* reason, std::uint64_t cooldownMs)
{
    const std::uint64_t now = GetTickCount64();
    std::uint64_t prev = g_lastCacheRefreshMs.load(std::memory_order_relaxed);
    if (cooldownMs > 0 && prev != 0 && now >= prev && (now - prev) < cooldownMs)
        return false;
    if (!g_lastCacheRefreshMs.compare_exchange_strong(prev, now, std::memory_order_relaxed))
        return false;
    TargetProcess.RefreshAllCaches();
    static std::uint64_t lastLogMs = 0;
    if (now - lastLogMs >= 5000) {
        lastLogMs = now;
        DLOG("DmaCacheRefresh: reason=%s", reason ? reason : "manual");
    }
    return true;
}

static void ClearEntityCaches();

// Periodically verify that ROB RUN.exe is still alive with the same PID,
// independent of whether cached root pointers are non-zero. Under MemProcFS
// -norefresh, freed physical pages can remain readable via stale page tables
// after the game exits/restarts, so root pointers may stay non-zero even when
// the old process is gone.
static std::uintptr_t ReadPtrFresh(std::uintptr_t address)
{
    std::uintptr_t value = 0;
    return RobRunMemory::ReadBytesFresh(address, &value, sizeof(value)) ? value : 0;
}

static std::uint64_t g_lastPidCheckMs = 0;
static std::uint64_t g_lastLiveChangeMs = 0;
static std::uint64_t g_lastLiveHash = 0;
static std::atomic<bool> g_reattachSignaled{ false };

static void ResetProcessWatchdog()
{
    g_lastPidCheckMs = 0;
    g_lastLiveChangeMs = 0;
    g_lastLiveHash = 0;
    g_reattachSignaled.store(false, std::memory_order_release);
}

static bool CheckGameProcessAlive(bool force = false)
{
    const std::uint64_t now = GetTickCount64();
    if (!force && g_lastPidCheckMs != 0 && now >= g_lastPidCheckMs && (now - g_lastPidCheckMs) < 3000)
        return true;
    g_lastPidCheckMs = now;

    const int attachedPid = TargetProcess.GetPID();
    if (attachedPid <= 0)
        return true;

    TargetProcess.RefreshProcessesFast();
    const DWORD livePid = TargetProcess.GetPidFromName("ROB RUN.exe");
    if (livePid == 0 || static_cast<int>(livePid) != attachedPid) {
        if (!g_reattachSignaled.exchange(true, std::memory_order_acq_rel)) {
            DLOG("ProcessWatchdog: ROB RUN.exe PID changed (%d -> %lu), requesting re-attach",
                 attachedPid, static_cast<unsigned long>(livePid));
            VeoxDiag("watchdog: ROB RUN.exe pid %d -> %lu, auto re-attaching",
                 attachedPid, static_cast<unsigned long>(livePid));
            std::printf("[Reader] ROB RUN.exe restarted/closed (pid %d -> %lu) — auto re-attaching...\n",
                attachedPid, static_cast<unsigned long>(livePid));
            RequestReattach();
        }
        return false;
    }
    g_reattachSignaled.store(false, std::memory_order_release);
    return true;
}

// city_colliders.bvh only matches the City map. On non-City maps (Hub, Cottage,
// etc.), raycasting against City geometry produces false occlusions where every
// player appears behind a wall.
static bool IsBvhActiveForCurrentLocation()
{
    std::string loc;
    {
        std::lock_guard<std::mutex> lk(locationMutex);
        loc = cachedLocationId;
    }
    if (loc.empty())
        return true; // location not read yet -> allow BVH
    std::string lower = loc;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower.find("city") != std::string::npos;
}

static void MarkSessionChanged()
{
    ++sessionGeneration;
    g_cameraInvalidated.store(true, std::memory_order_release);
    MaybeRefreshDmaCaches("sessionChanged", 0);
    DLOG("SessionChange: gen=%llu bm=0x%llx be=0x%llx lc=0x%llx game=0x%llx",
         (unsigned long long)sessionGeneration,
         (unsigned long long)cachedBattleModel,
         (unsigned long long)cachedBattleEntities,
         (unsigned long long)cachedLocalCharacter,
         (unsigned long long)cachedGame);
    sessionChangedMs = GetTickCount64();
    cachedSlots.clear();
    cachedViews.clear();
    cachedViewOwner.clear();
    viewOwnerMisses.clear();
    ClearEntityCaches();
    g_slotIdentityCache.clear();
    g_publicIdToNick.clear();
    g_roleCache.clear();
    g_weaponDescCache.clear();
    viewsRefreshed = false;
    slotRefreshStamp = 0;
    viewRefreshStamp = 0;
    cameraResolveStamp = 0;
    localChainStamp = 0;
    fullResolveStamp = 0;
    g_party = PartyMemberIds{};
    g_partyStamp = 0;
}

// Called once per pass after resolution. Any change in the scene roots means
// the previous pointers may now point at freed/reused memory.
static void NoteSessionState()
{
    if (!lastSessionValid) {
        lastSessionValid = true;
        lastBattleModel = cachedBattleModel;
        lastBattleEntities = cachedBattleEntities;
        lastCameraModel = cachedCameraModel;
        lastLocalCharacter = cachedLocalCharacter;
        lastUpdater = cachedUpdater;
        lastLocalEntityId = localEntityId;
        return;
    }

    if (cachedBattleModel == lastBattleModel &&
        cachedBattleEntities == lastBattleEntities &&
        cachedCameraModel == lastCameraModel &&
        cachedLocalCharacter == lastLocalCharacter &&
        cachedUpdater == lastUpdater &&
        localEntityId == lastLocalEntityId)
        return;

    MarkSessionChanged();
    lastBattleModel = cachedBattleModel;
    lastBattleEntities = cachedBattleEntities;
    lastCameraModel = cachedCameraModel;
    lastLocalCharacter = cachedLocalCharacter;
    lastUpdater = cachedUpdater;
    lastLocalEntityId = localEntityId;
}

static std::vector<EntityData> entities;
// World/inventory state is built on a separate worker thread and published as
// immutable shared objects. Copying the whole state into every snapshot (and,
// worse, reading it inline in the player pass) made the ESP freeze while the
// dictionaries were walked.
static std::mutex g_worldMutex;
static std::shared_ptr<const RobRunWorld::State> g_worldState =
    std::make_shared<RobRunWorld::State>();
static std::shared_ptr<const RobRunInventory::State> g_inventoryState =
    std::make_shared<RobRunInventory::State>();
static std::shared_ptr<const RobRunWorld::PlotState> g_plotState =
    std::make_shared<RobRunWorld::PlotState>();
static std::uint64_t worldRefreshStamp = 0;
static std::uint64_t inventoryRefreshStamp = 0;
static RobRunCamera::CameraState cameraState;
static InGameMapState inGameMapState;
static std::vector<FastVehicle> fastVehicles;
struct FastVehicleEntry {
    int id = 0;
    RobRunWorld::Kind kind = RobRunWorld::Kind::Car;
    std::uintptr_t model = 0;
    std::uintptr_t posAddr = 0; // direct Vector3 address (wrapper+0x10 or transform+0x10)
    std::uint64_t ownerPublicId = 0;
    std::string ownerRoleName;
    bool hasDriver = false;
};
static std::vector<FastVehicleEntry> g_fastVehMeta;
static std::uint64_t g_fastVehMetaStamp = 0;
static Vector3 localPosition{};
static float localYaw = 0.f;
static float localPitch = 0.f;
static std::uint32_t failureCount = 0;
static std::uint32_t poseFailureCount = 0;
static std::uint32_t boneTotalAcc = 0;
static std::uint32_t boneInvalidAcc = 0;
static std::uint32_t boneNormZeroAcc = 0;
static std::uint32_t boneNormBadAcc = 0;
static std::uint32_t bonePosInfAcc = 0;
static std::uint32_t boneChainDropAcc = 0;
static std::uint32_t poseScatterOkAcc = 0;
static std::uint32_t poseScatterFailAcc = 0;
static std::uint32_t poseFreshOkAcc = 0;
static std::atomic<bool> readCatalog{ false };
static int updateCounter = 0;
static int gameRetryCountdown = 0;
static int updaterRetryCountdown = 0;
static int battleEntitiesRetryCountdown = 0;

static Matrix4x4 fallbackViewProjection{};
static bool fallbackViewProjectionValid = false;

struct CrosshairState {
    std::uintptr_t updater  = 0;
    std::uintptr_t original = 0;
    bool hidden              = false;
};
static CrosshairState g_crosshairState;

static bool HideDefaultCrosshair(std::uintptr_t game);
static bool RestoreDefaultCrosshair();

// Slow-changing per-entity data (health, role, vehicle, identity). Reading the
// primitive wrappers and strings costs far more than a position read, so it is
// refreshed a few times per second and copied into every snapshot.
struct DetailCacheEntry {
    std::uintptr_t characterModel = 0;
    std::uintptr_t transform = 0;
    std::uint64_t stamp = 0;
    std::uint64_t publicId = 0;
    float health = -1.f;
    float maxHealth = -1.f;
    float criticalHealth = -1.f;
    bool healthKnown = false;
    bool isDead = false;
    bool isSprinting = false;
    bool isCrouching = false;
    bool inVehicle = false;
    int vehicleId = 0;
    int role = 0;
    std::string roleName;
    std::wstring nickname;
    std::wstring clanTag;
    std::wstring weaponName;
    std::string nicknameUtf8;
    std::string clanTagUtf8;
    std::string weaponNameUtf8;
    int weaponIndex = -1;
    bool isFiring = false;
    bool isReloading = false;
    bool hasArmor = false;
    float armor = 0.f;
    float stamina = -1.f;
    std::int32_t bagValue = 0;
    std::int32_t bagCash = 0;
    std::int32_t bagItems = 0;
    std::int32_t equipValue = 0;
    bool isKnockedDown = false;
    bool isKnockedOut = false;
    bool isSurrendered = false;
    bool isHandcuffed = false;
    std::string reloadPhase;
};
// Worker-owned slow-changing detail cache (health, role, identity). Same
// threading contract as cachedViews: worker writes only, render thread reads
// the published Snapshot.
static std::unordered_map<std::int32_t, DetailCacheEntry> detailCache;

// Rig layout + last sampled pose. The layout (matrix array, parent table and
// selected bones) only changes when the view changes, so it is resolved once
// and then sampled with a single read per pass.
struct RigCacheEntry {
    std::uintptr_t view = 0;
    std::uint64_t layoutStamp = 0;
    std::uint64_t fallbackStamp = 0;
    bool layoutValid = false;
    Skeleton::RigLayout layout;
    std::vector<Skeleton::BoneSegment> bones;
    Skeleton::BonePoints points;
    // Full local TRS of every bone, used to tell whether the animator is
    // still evaluating this character. Unity skeletal animation is driven by
    // local *rotations*; comparing positions alone marks most of the skeleton
    // as frozen, which is why the old check only lit up the torso.
    std::vector<Skeleton::BoneLocal> curLocal;
    std::vector<Skeleton::BoneLocal> prevLocal;
    bool prevLocalValid = false;
    // Animator state is character-wide: Unity culls whole renderers, never
    // individual bones. Aggregate every bone, then colour all segments from
    // this single result so the ESP cannot show a half-visible body.
    std::uint64_t lastMotionTick = 0;
    bool animatorActive = true;
    // Consecutive failed fresh pose reads. A single device hiccup must not
    // discard the rig, but a persistent failure means the matrix array moved
    // and the layout needs rebuilding.
    int poseFailures = 0;
    std::uint64_t poseFailureSince = 0;
    // Worker-side exponential smoothing of the published pose. The overlay
    // draws these directly; there is no render-thread interpolation, so the
    // latency between the sampled pose and the drawn pose is at most one pass
    // and never extrapolates past the newest sample.
    std::vector<Skeleton::BoneSegment> smoothedBones;
    Skeleton::BonePoints smoothedPoints;
    bool smoothedValid = false;
    Vector3 lastSampledPos{};
    std::vector<Matrix4x4> worldMatrices;
    std::vector<Matrix4x4> smoothedWorldMatrices;
    // 3-sample ring buffer of pelvis-relative bone postures. Taking the
    // 3-sample temporal median on (bone - pelvis) and adding back the live,
    // undelayed pelvis world position keeps moving players at full worker
    // refresh rate (0 ms position lag) while mathematically erasing isolated
    // 1-frame pre-LateUpdate / torn-DMA flickers of any amplitude.
    std::vector<Skeleton::BoneSegment> relBoneHist[3];
    Skeleton::BonePoints relPtsHist[3]{};
    int poseHistCount = 0;
    int poseHistCursor = 0;
    int upgradeAttempts = 0;
};
static std::unordered_map<std::int32_t, RigCacheEntry> rigCache;

struct RectTransformCache {
    std::uintptr_t managed = 0;
    std::uint64_t stampMs = 0;
    bool valid = false;
    float uiMinX = 0.f, uiMinY = 0.f, uiMaxX = 0.f, uiMaxY = 0.f;
    float uiCenterX = 0.f, uiCenterY = 0.f;
    float uiScaleX = 1.f, uiScaleY = 1.f;
    float uiScreenW = 0.f, uiScreenH = 0.f;
};
static std::uintptr_t cachedMapDialogModel = 0;
static std::uintptr_t cachedMapConfigModel = 0;
static std::uintptr_t cachedMapFullVar = 0;
static std::uintptr_t cachedMapMiniVar = 0;
static std::uintptr_t cachedMapModelPtr = 0;
static RectTransformCache g_miniRectCache{};
static RectTransformCache g_fullRectCache{};
static std::int32_t s_rectTransformRectOffset = -1;
static std::atomic<std::uintptr_t> g_liveFullMapVarPtr{ 0 };
static std::mutex g_liveFullMapMutex;
static struct {
    bool valid = false;
    Vector2 pos{};
    float rot = 0.f;
    float camSize = 0.f;
} g_liveFullMap{};
static std::atomic<bool> g_fullMapFastMode{ false };

static void ClearEntityCaches()
{
    rigCache.clear();
    detailCache.clear();
    cachedMapDialogModel = 0;
    cachedMapConfigModel = 0;
    cachedMapFullVar = 0;
    cachedMapMiniVar = 0;
    cachedMapModelPtr = 0;
    g_miniRectCache = RectTransformCache{};
    g_fullRectCache = RectTransformCache{};
    inGameMapState = InGameMapState{};
    fastVehicles.clear();
    g_fastVehMeta.clear();
    g_fastVehMetaStamp = 0;
    g_liveFullMapVarPtr.store(0, std::memory_order_release);
    g_fullMapFastMode.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(g_liveFullMapMutex);
        g_liveFullMap.valid = false;
    }
}

static void PruneCaches()
{
    // Entity IDs are stable within a match but the caches should not grow
    // without bound across map changes. Clearing is cheap: layouts rebuild on
    // the next pass.
    if (rigCache.size() > 1024) rigCache.clear();
    if (detailCache.size() > 1024) detailCache.clear();
    if (g_slotIdentityCache.size() > 512) g_slotIdentityCache.clear();
    if (g_roleCache.size() > 512) g_roleCache.clear();
    if (g_weaponDescCache.size() > 512) g_weaponDescCache.clear();
}

// Worker-published snapshot. Only the worker writes; any thread may read.
static std::mutex g_snapshotMutex;
static std::shared_ptr<const Snapshot> g_snapshot = std::make_shared<Snapshot>();
static std::atomic<bool> g_workerStop{ false };
static std::thread g_workerThread;
static std::atomic<bool> g_worldWorkerStop{ false };
static std::thread g_worldWorkerThread;

// Dedicated camera polling thread. Reads the native Unity camera at ~0.5 ms
// intervals and publishes into a lock-free seqlock so the render thread never
// blocks on DMA or mutexes while panning.
static std::atomic<bool> g_cameraStop{ false };
static std::atomic<bool> g_cameraRunning{ false };
static std::thread g_cameraThread;
static std::atomic<std::uint64_t> g_perfWorkerPasses{ 0 };
static std::atomic<std::uint64_t> g_perfWorkerUs{ 0 };
static std::atomic<std::uint64_t> g_perfWorkerMaxUs{ 0 };
static std::atomic<std::uint64_t> g_perfP1Us{ 0 };
static std::atomic<std::uint64_t> g_perfP2Us{ 0 };
static std::atomic<std::uint64_t> g_perfP3Us{ 0 };
static std::atomic<std::uint64_t> g_perfDetUs{ 0 };
static std::atomic<std::uint64_t> g_perfP5Us{ 0 };
static std::atomic<std::uint64_t> g_perfRebuilds{ 0 };
static std::atomic<std::uint64_t> g_perfFallbacks{ 0 };
static std::atomic<int> g_perfSkels{ 0 };
static std::atomic<int> g_perfLiving{ 0 };
static std::atomic<std::uint64_t> g_perfCamReads{ 0 };
static std::atomic<std::uint64_t> g_perfCamPhys{ 0 };
static std::atomic<std::uint64_t> g_perfCamChanges{ 0 };
static std::atomic<std::uint64_t> g_perfCamRttUs{ 0 };
static std::atomic<std::uint64_t> g_perfCamRttMaxUs{ 0 };
static bool g_lastPassHadEntities = false;
static void StartCameraWorker();
static void StopCameraWorker();

// Worker publishes cachedCameraModel via this atomic; the camera thread reads
// either the native Unity Camera (fast path) or BattleCameraModel (fallback).
static std::atomic<std::uintptr_t> g_renderCameraModel{ 0 };
static std::mutex g_freshCameraMutex;
static RobRunCamera::CameraState g_freshCamera;

// Native Unity main-camera view-projection, published via a single-writer
// lock-free seqlock. Memory aim rewrites the managed BattleCameraModel block,
// so the native Camera component is the camera the GPU actually used and is
// preferred whenever readable; the managed block stays as the fallback.
struct NativeCameraSnapshot {
    Matrix4x4 view{};
    Matrix4x4 projection{};
    Vector3 position{};
    Vector3 forward{};
    float positionError = -1.f;
    float forwardDot = -1.f;
    std::uint64_t stampUs = 0;
    bool valid = false;
    bool poseValid = false;
};

static std::atomic<std::uint64_t> g_nativeCamSeq{ 0 };
static NativeCameraSnapshot g_nativeCamBuf{};
static std::mutex g_nativeCamWriteMutex;

static void PublishNativeCamera(const NativeCameraSnapshot& next)
{
    std::lock_guard<std::mutex> lock(g_nativeCamWriteMutex);
    const std::uint64_t seq = g_nativeCamSeq.load(std::memory_order_relaxed);
    g_nativeCamSeq.store(seq + 1, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);
    g_nativeCamBuf = next;
    std::atomic_thread_fence(std::memory_order_release);
    g_nativeCamSeq.store(seq + 2, std::memory_order_release);
}

static NativeCameraSnapshot LoadNativeCamera()
{
    NativeCameraSnapshot out{};
    for (int tries = 0; tries < 64; ++tries) {
        const std::uint64_t s1 = g_nativeCamSeq.load(std::memory_order_acquire);
        if (s1 & 1u) {
            _mm_pause();
            continue;
        }
        out = g_nativeCamBuf;
        std::atomic_thread_fence(std::memory_order_acquire);
        const std::uint64_t s2 = g_nativeCamSeq.load(std::memory_order_acquire);
        if (s1 == s2)
            return out;
    }
    std::lock_guard<std::mutex> lock(g_nativeCamWriteMutex);
    return g_nativeCamBuf;
}

static std::mutex g_bvhMutex;
static std::shared_ptr<const CollisionBVH::Scene> g_bvhScene =
    std::make_shared<CollisionBVH::Scene>();
static std::string g_bvhRequestedPath;
static std::uint64_t g_bvhLastAttemptMs = 0;
static CollisionBVH::DynamicOccluders g_dynOccluders;
static std::mutex g_dynOccludersMutex;

static void UpdateDynamicOccluders(const std::shared_ptr<const RobRunWorld::State>& world)
{
    if (!world) return;
    CollisionBVH::DynamicOccluders dyn;
    dyn.vehicles.reserve(world->entities.size());
    dyn.smokes.reserve(world->entities.size());

    for (const auto& e : world->entities) {
        if (e.kind == RobRunWorld::Kind::Vehicle ||
            e.kind == RobRunWorld::Kind::Car ||
            e.kind == RobRunWorld::Kind::Helicopter)
        {
            CollisionBVH::VehicleOBB v;
            v.center = { e.position.x, e.position.y + 0.95f, e.position.z };
            v.yawRad = e.yaw * (3.14159265f / 180.0f);
            v.isHelicopter = (e.kind == RobRunWorld::Kind::Helicopter);
            if (v.isHelicopter) {
                v.halfExtents = { 1.5f, 1.8f, 4.0f };
                v.center.y = e.position.y + 1.8f;
            } else {
                v.halfExtents = { 1.1f, 0.95f, 2.4f };
            }
            v.InitCache();
            dyn.vehicles.push_back(v);
        }
        else if (e.kind == RobRunWorld::Kind::Grenade) {
            CollisionBVH::SmokeSphere smk;
            smk.center = { e.position.x, e.position.y + 0.5f, e.position.z };
            smk.radius = 4.5f;
            dyn.smokes.push_back(smk);
        }
    }

    std::lock_guard<std::mutex> lock(g_dynOccludersMutex);
    g_dynOccluders = std::move(dyn);
}

CollisionBVH::DynamicOccluders GetDynamicOccluders()
{
    std::lock_guard<std::mutex> lock(g_dynOccludersMutex);
    return g_dynOccluders;
}

static std::shared_ptr<const CollisionBVH::Scene> LoadBvhSnapshot()
{
    std::lock_guard<std::mutex> lock(g_bvhMutex);
    return g_bvhScene;
}

static void TryLoadBvh(bool force = false)
{
    const std::string path = ConfigInstance.Player.BvhPath.empty()
        ? std::string("city_colliders.bvh")
        : ConfigInstance.Player.BvhPath;

    const std::uint64_t now = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(g_bvhMutex);
        if (!force)
        {
            if (g_bvhScene && g_bvhScene->valid && path == g_bvhRequestedPath)
                return;
            if (g_bvhLastAttemptMs != 0 && (now - g_bvhLastAttemptMs) < 3000 && path == g_bvhRequestedPath)
                return;
        }
        g_bvhLastAttemptMs = now ? now : 1;
        g_bvhRequestedPath = path;
    }

    auto nextScene = std::make_shared<CollisionBVH::Scene>();
    CollisionBVH::Load(path, *nextScene);

    std::lock_guard<std::mutex> lock(g_bvhMutex);
    g_bvhScene = std::move(nextScene);
}

static bool DmaRead(std::uint64_t address, void* buffer, std::size_t size)
{
    return TargetProcess.Read(static_cast<std::uintptr_t>(address), buffer, size);
}

// Uncached read for fast-changing values. Kept separate from DmaRead so
// static pointer chains still benefit from VMM's cache.
static bool DmaReadFresh(std::uint64_t address, void* buffer, std::size_t size)
{
    return TargetProcess.ReadFresh(static_cast<std::uintptr_t>(address), buffer, size);
}

// High-resolution monotonic clock for pose timestamps. GetTickCount64 is far
// too coarse (~15.6 ms) to interpolate a 30 Hz pose stream.
static std::uint64_t SteadyMicros()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Views only exist for streamed (nearby) players, and the dictionary key is
// not a player id: a view has to be matched to the player whose replicated
// position it reports. Each view's root transform is the character root, so
// its world position is compared with every player's position and the nearest
// pairs are claimed one at a time.
//
// The pairing is sticky. Rebuilding it from scratch on every refresh made the
// bones blink whenever a single root/position read failed or a player drifted
// past the match radius for one pass, so an existing pair is kept until its
// view disappears, its player has been absent for several passes, or the two
// have moved far apart. New pairs are only formed within kMatchRadius; old
// ones are only dropped beyond kReleaseRadius.
static void MatchViewsToEntities()
{
    if (cachedViews.empty() || entities.empty()) return;

    std::unordered_map<std::int32_t, std::size_t> entityIndex;
    entityIndex.reserve(entities.size());
    for (std::size_t e = 0; e < entities.size(); ++e)
    {
        if (entities[e].entityId >= 0)
            entityIndex.emplace(entities[e].entityId, e);
    }

    struct ViewCandidate {
        std::uintptr_t view = 0;
        Vector3 position{};
        bool hasPosition = false;
        bool claimed = false;
    };
    std::vector<ViewCandidate> views;
    views.reserve(cachedViews.size());
    // Lazy view position resolution: initialize candidate list without
    // querying RootWorldPosition up front. Step 0 (direct key match) matches
    // almost all entities instantly without needing any root position reads.
    for (const auto& entry : cachedViews)
    {
        ViewCandidate candidate;
        candidate.view = entry.second;
        candidate.hasPosition = false;
        candidate.claimed = false;
        views.push_back(candidate);
    }

    auto ensureViewPosition = [&](ViewCandidate& cand) -> bool {
        if (cand.hasPosition) return true;
        Skeleton::Vec root{};
        if (cand.view > 0x10000 &&
            Skeleton::RootWorldPosition(cand.view, DmaRead, root))
        {
            cand.position = Vector3{ root.x, root.y, root.z };
            cand.hasPosition = RobRunMemory::Finite(cand.position);
        }
        return cand.hasPosition;
    };

    std::unordered_map<std::uintptr_t, std::size_t> viewIndex;
    viewIndex.reserve(views.size());
    for (std::size_t v = 0; v < views.size(); ++v)
        viewIndex.emplace(views[v].view, v);

    // A player can cover several metres between two 400 ms refreshes, so the
    // acquire radius is generous. The release radius is far larger so only a
    // genuinely different player breaks an existing pair.
    constexpr float kMatchRadius = 25.f;
    constexpr float kReleaseRadius = 80.f;
    constexpr int kMaxMisses = 3;

    std::unordered_map<std::int32_t, std::uintptr_t> nextOwner;
    std::unordered_map<std::int32_t, int> nextMisses;
    std::unordered_map<std::int32_t, std::uintptr_t> claimedByEntity;

    // 0. Direct dictionary key match: _characterEntityViews is keyed by entity ID.
    for (std::size_t e = 0; e < entities.size(); ++e)
    {
        const auto& entity = entities[e];
        auto it = cachedViews.find(entity.entityId);
        if (it != cachedViews.end() && it->second > 0x10000)
        {
            auto vIt = viewIndex.find(it->second);
            if (vIt != viewIndex.end() && !views[vIt->second].claimed)
            {
                views[vIt->second].claimed = true;
                nextOwner.emplace(entity.entityId, it->second);
                claimedByEntity.emplace(entity.entityId, it->second);
            }
        }
    }

    // 1. Carry over pairs that are still plausible.
    for (const auto& owner : cachedViewOwner)
    {
        const std::int32_t entityId = owner.first;
        if (claimedByEntity.find(entityId) != claimedByEntity.end())
            continue;

        const std::uintptr_t view = owner.second;
        const auto viewIt = viewIndex.find(view);
        if (viewIt == viewIndex.end())
            continue; // view streamed out or was reallocated: pair is over

        const auto entityIt = entityIndex.find(entityId);
        if (entityIt == entityIndex.end())
        {
            // The player vanished from the dictionary for a pass. Keep the rig
            // reserved for a moment so a dictionary hiccup does not blink the
            // skeleton off.
            const auto missIt = viewOwnerMisses.find(entityId);
            const int misses =
                (missIt == viewOwnerMisses.end() ? 0 : missIt->second) + 1;
            if (misses < kMaxMisses)
            {
                nextOwner.emplace(entityId, view);
                nextMisses.emplace(entityId, misses);
                views[viewIt->second].claimed = true;
                claimedByEntity.emplace(entityId, view);
            }
            continue;
        }

        auto& candidate = views[viewIt->second];
        candidate.claimed = true;
        if (ensureViewPosition(candidate))
        {
            const auto& entity = entities[entityIt->second];
            // Fast vehicles easily travel > 12m between 400ms scans.
            // Never unpair an existing entity in a vehicle by distance!
            if (!entity.inVehicle &&
                RobRunMemory::Finite(entity.position) &&
                !entity.position.IsZero() &&
                Distance(entity.position, candidate.position) > kReleaseRadius)
            {
                // The positions have clearly diverged, so this rig belongs to
                // somebody else now. Free it for re-pairing below.
                candidate.claimed = false;
                continue;
            }
        }
        nextOwner.emplace(entityId, view);
        claimedByEntity.emplace(entityId, view);
    }

    // 2. Pair remaining views with remaining players, closest first, so two
    //    players standing together can never share one rig.
    struct Pair { float distance = 0.f; std::size_t entity = 0; std::size_t view = 0; };
    std::vector<Pair> pairs;
    for (std::size_t e = 0; e < entities.size(); ++e)
    {
        const auto& entity = entities[e];
        if (claimedByEntity.find(entity.entityId) != claimedByEntity.end())
            continue;
        if (!RobRunMemory::Finite(entity.position) || entity.position.IsZero())
            continue;
        for (std::size_t v = 0; v < views.size(); ++v)
        {
            if (views[v].claimed || !ensureViewPosition(views[v])) continue;
            const float distance = Distance(entity.position, views[v].position);
            if (distance <= kMatchRadius)
                pairs.push_back(Pair{ distance, e, v });
        }
    }
    std::sort(pairs.begin(), pairs.end(),
        [](const Pair& a, const Pair& b) { return a.distance < b.distance; });
    for (const Pair& pair : pairs)
    {
        const std::int32_t entityId = entities[pair.entity].entityId;
        if (views[pair.view].claimed ||
            claimedByEntity.find(entityId) != claimedByEntity.end())
            continue;
        views[pair.view].claimed = true;
        claimedByEntity.emplace(entityId, views[pair.view].view);
        nextOwner.emplace(entityId, views[pair.view].view);
    }

    // 3. Last resort: keep the dictionary key for any view whose root could
    //    not be read or matched, so a rig is never lost outright while the
    //    character spawns, is pooled, or its hierarchy is rebuilt.
    for (const auto& entry : cachedViews)
    {
        if (entry.second <= 0x10000) continue;
        const auto viewIt = viewIndex.find(entry.second);
        if (viewIt != viewIndex.end() && views[viewIt->second].claimed) continue;
        if (entityIndex.find(entry.first) == entityIndex.end()) continue;
        if (claimedByEntity.find(entry.first) != claimedByEntity.end()) continue;
        if (viewIt != viewIndex.end())
            views[viewIt->second].claimed = true;
        claimedByEntity.emplace(entry.first, entry.second);
        nextOwner.emplace(entry.first, entry.second);
    }

    cachedViewOwner.swap(nextOwner);
    viewOwnerMisses.swap(nextMisses);
}

static void PublishSnapshot()
{
    auto snapshot = std::make_shared<Snapshot>();
    g_lastPassHadEntities = !entities.empty();
    snapshot->entities = std::move(entities);
    snapshot->fastVehicles = fastVehicles;
    {
        std::lock_guard<std::mutex> lock(g_worldMutex);
        snapshot->world = g_worldState;
        snapshot->plots = g_plotState;
        snapshot->inventory = g_inventoryState;
    }
    snapshot->camera = cameraState;
    snapshot->inGameMap = inGameMapState;
    snapshot->localEntityId = localEntityId;
    snapshot->localPosition = localPosition;
    snapshot->localYaw = localYaw;
    snapshot->localPitch = localPitch;
    snapshot->game = cachedGame;
    snapshot->battleModel = cachedBattleModel;
    snapshot->localCharacter = cachedLocalCharacter;
    snapshot->cameraModel = cachedCameraModel;
    snapshot->lookDirectionModel = cachedLookDirectionModel;
    snapshot->lookDirDirect = cachedLookDirDirect;
    snapshot->playerRotationInput = cachedPlayerRotationInput;
    snapshot->battleEntities = cachedBattleEntities;
    snapshot->battleEntitiesUpdater = cachedUpdater;
    snapshot->failureCount = failureCount;
    snapshot->poseFailures = poseFailureCount;
    snapshot->boneTotal = boneTotalAcc;
    snapshot->boneInvalid = boneInvalidAcc;
    snapshot->boneNormZero = boneNormZeroAcc;
    snapshot->boneNormBad = boneNormBadAcc;
    snapshot->bonePosInf = bonePosInfAcc;
    snapshot->boneChainDrop = boneChainDropAcc;
    snapshot->poseScatterOk = poseScatterOkAcc;
    snapshot->poseScatterFail = poseScatterFailAcc;
    snapshot->poseFreshOk = poseFreshOkAcc;
    snapshot->stampMs = GetTickCount64();
    snapshot->stampUs = SteadyMicros();
    snapshot->sessionGeneration = sessionGeneration;
    snapshot->sessionChangedMs = sessionChangedMs;
    for (auto& entity : snapshot->entities) {
        if (entity.entityId != localEntityId && !entity.isDead &&
            RobRunMemory::Finite(localPosition) && !localPosition.IsZero()) {
            const float dx = localPosition.x - entity.position.x;
            const float dy = localPosition.y - entity.position.y;
            const float dz = localPosition.z - entity.position.z;
            const float distSq = dx * dx + dy * dy + dz * dz;
            if (distSq > 0.01f) {
                const float invDist = 1.f / std::sqrt(distSq);
                const float dot = entity.lookDirection.x * dx * invDist +
                                  entity.lookDirection.y * dy * invDist +
                                  entity.lookDirection.z * dz * invDist;
                if (dot > 0.90f) {
                    entity.isAimingAtLocal = true;
                }
            }
        }
        if (!entity.bones.empty()) ++snapshot->skeletonCount;
        if (entity.bonePoints.valid) ++snapshot->poseCount;
    }
    snapshot->fallbackViewProjection = fallbackViewProjection;
    snapshot->fallbackViewProjectionValid = fallbackViewProjectionValid;

    std::lock_guard<std::mutex> lock(g_snapshotMutex);
    g_snapshot = std::move(snapshot);
}

void Shutdown()
{
    StopCameraWorker();
    g_workerStop.store(true, std::memory_order_relaxed);
    g_worldWorkerStop.store(true, std::memory_order_relaxed);
    if (g_workerThread.joinable())
        g_workerThread.join();
    if (g_worldWorkerThread.joinable())
        g_worldWorkerThread.join();
    // Restore after stopping workers so g_workerThread cannot re-hide it.
    RestoreDefaultCrosshair();

    {
        std::lock_guard<std::mutex> lock(g_snapshotMutex);
        g_snapshot = std::make_shared<Snapshot>();
    }
    {
        std::lock_guard<std::mutex> lock(g_worldMutex);
        g_worldState = std::make_shared<RobRunWorld::State>();
        g_plotState = std::make_shared<RobRunWorld::PlotState>();
        g_inventoryState = std::make_shared<RobRunInventory::State>();
    }
    g_renderCameraModel.store(0, std::memory_order_relaxed);
    g_cameraInvalidated.store(false, std::memory_order_relaxed);
    CameraFinder::InvalidateCache();
    {
        std::lock_guard<std::mutex> lock(g_freshCameraMutex);
        g_freshCamera = RobRunCamera::CameraState{};
    }
    PublishNativeCamera(NativeCameraSnapshot{});
    ClearEntityCaches();
    ResetProcessWatchdog();
    gaBase = 0;
    upBase = 0;
}

void Init(std::uintptr_t gameAssemblyBase, std::uintptr_t unityPlayerBase)
{
    DLOG("RobRunReader::Init(ga=0x%llx, up=0x%llx)", (unsigned long long)gameAssemblyBase, (unsigned long long)unityPlayerBase);
    Shutdown();
    ClearEntityCaches();
    ResetProcessWatchdog();
    DLOG("RobRunReader::Init: Shutdown done");

    gaBase = gameAssemblyBase;
    upBase = unityPlayerBase;
    Il2Cpp::Init(gameAssemblyBase);
    RobRunCamera::Init(gameAssemblyBase);
    RobRunInventory::Init(gameAssemblyBase);
    RobRunWorld::Init(gameAssemblyBase);
    DLOG("RobRunReader::Init: sub-inits done");
    cachedGame = 0;
    cachedBattleModel = 0;
    cachedBattleEntities = 0;
    cachedCameraModel = 0;
    cachedLocalCharacter = 0;
    cachedLookDirectionModel = 0;
    cachedLookDirDirect = 0;
    cachedPlayerRotationInput = 0;
    cachedUpdater = 0;
    sessionGeneration = 0;
    sessionChangedMs = 0;
    lastBattleModel = 0;
    lastBattleEntities = 0;
    lastCameraModel = 0;
    lastLocalCharacter = 0;
    lastUpdater = 0;
    lastLocalEntityId = -1;
    lastSessionValid = false;
    failureCount = 0;
    updateCounter = 0;
    gameRetryCountdown = 0;
    updaterRetryCountdown = 0;
    battleEntitiesRetryCountdown = 0;
    entities.clear();
    detailCache.clear();
    rigCache.clear();
    cachedSlots.clear();
    cachedViews.clear();
    cachedViewOwner.clear();
    viewOwnerMisses.clear();
    viewsRefreshed = false;
    slotRefreshStamp = 0;
    viewRefreshStamp = 0;
    cameraResolveStamp = 0;
    localChainStamp = 0;
    fullResolveStamp = 0;
    {
        std::lock_guard<std::mutex> lock(g_worldMutex);
        g_worldState = std::make_shared<RobRunWorld::State>();
        g_plotState = std::make_shared<RobRunWorld::PlotState>();
        g_inventoryState = std::make_shared<RobRunInventory::State>();
    }
    worldRefreshStamp = 0;
    inventoryRefreshStamp = 0;
    cameraState = RobRunCamera::CameraState{};
    localEntityId = -1;
    localPosition = Vector3();
    fallbackViewProjection = Matrix4x4{};
    fallbackViewProjectionValid = false;
    { std::lock_guard<std::mutex> lk(locationMutex); cachedLocationId.clear(); }
    locationStamp = 0;

    g_renderCameraModel.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_freshCameraMutex);
        g_freshCamera = RobRunCamera::CameraState{};
    }

    DLOG("RobRunReader::Init: TryLoadBvh");
    TryLoadBvh(true);
    {
        const auto bvh = LoadBvhSnapshot();
        DLOG("RobRunReader::Init: BVH done, valid=%d", (bvh && bvh->valid) ? 1 : 0);
    }

    {
        std::lock_guard<std::mutex> lock(g_snapshotMutex);
        g_snapshot = std::make_shared<Snapshot>();
    }

    DLOG("RobRunReader::Init: starting worker threads");
    g_workerStop.store(false, std::memory_order_relaxed);
    g_workerThread = std::thread([] {
        DLOG("Worker thread started");
        std::uint64_t perfWinStartUs = SteadyMicros();
        while (!g_workerStop.load(std::memory_order_relaxed)) {
            const std::uint64_t t0 = SteadyMicros();
            Update();
            const std::uint64_t tEnd = SteadyMicros();
            const std::uint64_t dt = (tEnd >= t0) ? (tEnd - t0) : 0;
            g_perfWorkerPasses.fetch_add(1, std::memory_order_relaxed);
            g_perfWorkerUs.fetch_add(dt, std::memory_order_relaxed);
            std::uint64_t prevMax = g_perfWorkerMaxUs.load(std::memory_order_relaxed);
            while (dt > prevMax && !g_perfWorkerMaxUs.compare_exchange_weak(prevMax, dt, std::memory_order_relaxed)) {}

            // Emit periodic perf summary from the normal-priority worker thread
            // instead of blocking the THREAD_PRIORITY_HIGHEST camera thread on console I/O.
            if (tEnd >= perfWinStartUs && (tEnd - perfWinStartUs) >= 3'000'000ULL) {
                const std::uint64_t camReads = g_perfCamReads.exchange(0, std::memory_order_relaxed);
                if (camReads > 0) {
                    const double sec = static_cast<double>(tEnd - perfWinStartUs) / 1'000'000.0;
                    const std::uint64_t camPhys = g_perfCamPhys.exchange(0, std::memory_order_relaxed);
                    const std::uint64_t camChg = g_perfCamChanges.exchange(0, std::memory_order_relaxed);
                    const std::uint64_t camRttSum = g_perfCamRttUs.exchange(0, std::memory_order_relaxed);
                    const std::uint64_t camRttMax = g_perfCamRttMaxUs.exchange(0, std::memory_order_relaxed);
                    const double camHz = sec > 0.0 ? (camReads / sec) : 0.0;
                    const double chgHz = sec > 0.0 ? (camChg / sec) : 0.0;
                    const double physPct = 100.0 * camPhys / camReads;
                    const std::uint64_t avgUs = camRttSum / camReads;
                    const std::uint64_t wPasses = g_perfWorkerPasses.exchange(0, std::memory_order_relaxed);
                    const std::uint64_t wUs = g_perfWorkerUs.exchange(0, std::memory_order_relaxed);
                    const std::uint64_t wMaxUs = g_perfWorkerMaxUs.exchange(0, std::memory_order_relaxed);
                    const std::uint64_t p1Us = g_perfP1Us.exchange(0, std::memory_order_relaxed);
                    const std::uint64_t p2Us = g_perfP2Us.exchange(0, std::memory_order_relaxed);
                    const std::uint64_t p3Us = g_perfP3Us.exchange(0, std::memory_order_relaxed);
                    const std::uint64_t detUs = g_perfDetUs.exchange(0, std::memory_order_relaxed);
                    const std::uint64_t p5Us = g_perfP5Us.exchange(0, std::memory_order_relaxed);
                    const std::uint64_t rebs = g_perfRebuilds.exchange(0, std::memory_order_relaxed);
                    const std::uint64_t fbs = g_perfFallbacks.exchange(0, std::memory_order_relaxed);
                    const int skels = g_perfSkels.load(std::memory_order_relaxed);
                    const int living = g_perfLiving.load(std::memory_order_relaxed);
                    const double wHz = sec > 0.0 ? (wPasses / sec) : 0.0;
                    const double wMs = wPasses ? (static_cast<double>(wUs) / wPasses / 1000.0) : 0.0;
                    const double invP = wPasses ? (1.0 / (wPasses * 1000.0)) : 0.0;
                    std::printf("[Perf] cam=%.0fHz src=%s (phys=%.0f%% avg=%lluus max=%lluus chg=%.0fHz) | worker=%.0fHz (avg=%.1fms max=%.1fms p1/p2/p3/det/p5=%.1f/%.1f/%.1f/%.1f/%.1fms reb=%llu fb=%llu skel=%d/%d) | overlay=%.0ffps mode=%s cap=%d\n",
                        camHz, GetViewProjectionSource(), physPct,
                        (unsigned long long)avgUs, (unsigned long long)camRttMax, chgHz,
                        wHz, wMs, wMaxUs / 1000.0,
                        p1Us * invP, p2Us * invP, p3Us * invP, detUs * invP, p5Us * invP,
                        (unsigned long long)rebs, (unsigned long long)fbs, skels, living,
                        Overlay::CurrentFps(), Overlay::PresentModeName(), Overlay::FrameCap());
                }
                perfWinStartUs = tEnd;
            }

            // Adaptive sleep: 0 ms (yield only) when fullscreen map is open and
            // 3D ESP is hidden so map positions refresh at 300-500 Hz, 2 ms when
            // normal 3D entities are active, or 10 ms during empty lobbies.
            if (g_fullMapFastMode.load(std::memory_order_relaxed)) {
                SwitchToThread();
            } else {
                const int sleepMs = (cachedBattleModel > 0x10000 && g_lastPassHadEntities) ? 2 : 10;
                for (int i = 0; i < sleepMs && !g_workerStop.load(std::memory_order_relaxed); ++i)
                    Sleep(1);
            }
        }
    });

    DLOG("RobRunReader::Init: main worker launched");
    g_worldWorkerStop.store(false, std::memory_order_relaxed);
    g_worldWorkerThread = std::thread([] {
        DLOG("World worker thread started");
        static int wwCount = 0;
        while (!g_worldWorkerStop.load(std::memory_order_relaxed)) {
            try {
                ++wwCount;
                // Controlled partial MemProcFS cache expiry (paired with
                // -norefresh): purely in-RAM invalidation with zero background
                // FPGA stalls. Expire 33% data cache every tick (~0.6-1s) and
                // 33% TLB faster when no enemy is close on screen (~2.5-3s) vs
                // ~6-8s during close-range visibility.
                TargetProcess.RefreshMemCachePartial();
                const bool renderQuiet =
                    !g_renderAnyOnScreen.load(std::memory_order_relaxed) ||
                    g_renderNearestDist.load(std::memory_order_relaxed) > 40.f;
                const int tlbDiv = renderQuiet ? 4 : 10;
                if ((wwCount % tlbDiv) == 0)
                    TargetProcess.RefreshTlbCachePartial();
                const std::uint64_t gen = sessionGeneration;
                const std::uintptr_t bm = cachedBattleModel;
                const std::uintptr_t be = cachedBattleEntities;
                const std::uintptr_t lc = cachedLocalCharacter;
                if (bm > 0x10000) {
                    auto state = std::make_shared<RobRunWorld::State>();
                    const bool worldOk = RobRunWorld::Read(bm, be, *state);
                    if ((wwCount % 100) == 1)
                        DLOG("WorldWorker #%d: Read=%d entities=%zu gen=%llu",
                             wwCount, worldOk, state->entities.size(), (unsigned long long)gen);
                    if (worldOk && sessionGeneration == gen) {
                        // Resolve each world entity's LootFilter catalog index once here
                        // (~2 Hz) so the 144-240 Hz render thread never hashes or scans labels.
                        for (auto& we : state->entities)
                            we.catalogIndex = LootFilterConfig::FindCatalogIndex(we.kind, we.label);
                        std::lock_guard<std::mutex> lock(g_worldMutex);
                        g_worldState = std::move(state);
                        UpdateDynamicOccluders(g_worldState);
                    }
                    if (be > 0x10000 && sessionGeneration == gen) {
                        auto plots = std::make_shared<RobRunWorld::PlotState>();
                        if (RobRunWorld::ReadPlots(be, *plots) && sessionGeneration == gen) {
                            std::lock_guard<std::mutex> lock(g_worldMutex);
                            g_plotState = std::move(plots);
                        }
                    }
                }
                if (lc > 0x10000 && sessionGeneration == gen) {
                    auto inv = std::make_shared<RobRunInventory::State>();
                    if (RobRunInventory::Read(bm, lc, *inv, readCatalog.load()) && sessionGeneration == gen) {
                        std::lock_guard<std::mutex> lock(g_worldMutex);
                        g_inventoryState = std::move(inv);
                    }
                }
                // Session changed while we were reading — clear stale published state
                if (sessionGeneration != gen) {
                    DLOG("WorldWorker #%d: session changed mid-read (was %llu, now %llu)",
                         wwCount, (unsigned long long)gen, (unsigned long long)sessionGeneration);
                    std::lock_guard<std::mutex> lock(g_worldMutex);
                    g_worldState = std::make_shared<RobRunWorld::State>();
                    g_plotState = std::make_shared<RobRunWorld::PlotState>();
                    g_inventoryState = std::make_shared<RobRunInventory::State>();
                }
            } catch (const std::exception& e) {
                DLOG("WorldWorker EXCEPTION #%d: %s", wwCount, e.what());
            } catch (...) {
                DLOG("WorldWorker UNKNOWN EXCEPTION #%d", wwCount);
            }
            for (int i = 0; i < 50 && !g_worldWorkerStop.load(std::memory_order_relaxed); ++i)
                Sleep(10);
        }
    });

    StartCameraWorker();
    DLOG("RobRunReader::Init: complete, workers and camera thread launched");
}

static std::uintptr_t ReadPtr(std::uintptr_t address)
{
    return RobRunMemory::ReadPtr(address);
}

static bool IsClass(std::uintptr_t object, const char* name)
{
    return Il2Cpp::IsClass(object, name);
}

// Last-resort singleton resolution. The class-table walk is expensive over
// DMA, so it only runs when the TypeInfo slot path failed and at most once
// every three seconds. Positive results are cached by the caller.
static std::uint64_t lastClassFallbackTick = 0;
static bool ClassFallbackDue()
{
    const std::uint64_t now = GetTickCount64();
    if (lastClassFallbackTick != 0 && now - lastClassFallbackTick < 3000)
        return false;
    lastClassFallbackTick = now;
    return true;
}

static std::uintptr_t ResolveStaticSingleton(const char* className,
    const char* expectedClass)
{
    const std::uintptr_t klass = Il2Cpp::FindClassByName(className);
    if (klass <= 0x10000) return 0;
    const std::uintptr_t statics = Il2Cpp::StaticFields(klass);
    if (statics <= 0x10000) return 0;
    const std::uintptr_t instance = ReadPtr(statics);
    return IsClass(instance, expectedClass) ? instance : 0;
}

// --- GOM-based Game resolution -------------------------------------------
static std::uintptr_t FindGameObject(const char* targetName)
{
    if (!upBase) return 0;
    const std::uintptr_t gom = ReadPtr(upBase + 0x2230768);
    if (!gom) return 0;
    std::uintptr_t node = ReadPtr(gom + 0x20);
    const std::uintptr_t sentinel = gom + 0x18;

    for (int i = 0; node && node != sentinel && i < 8192; ++i) {
        const std::uintptr_t object = ReadPtr(node + 0x10);
        if (object) {
            const std::uintptr_t namePtr = ReadPtr(object + 0x50);
            if (namePtr > 0x10000) {
                char name[64]{};
                if (RobRunMemory::ReadBytes(namePtr, name, sizeof(name) - 1) &&
                    std::strstr(name, targetName)) {
                    return object;
                }
            }
        }
        node = ReadPtr(node + 0x08);
    }
    return 0;
}

static std::uintptr_t GetComponent(std::uintptr_t gameObject, const char* className)
{
    if (gameObject <= 0x10000) return 0;
    const std::uintptr_t componentArray = ReadPtr(gameObject + 0x20);
    const std::int32_t componentCount = RobRunMemory::ReadInt32(gameObject + 0x30);
    if (componentCount <= 0 || componentCount > 256) return 0;

    for (int i = 0; i < componentCount; ++i) {
        const std::uintptr_t component = ReadPtr(
            componentArray + static_cast<std::uintptr_t>(i) * 0x10 + 0x08);
        if (!component) continue;
        const std::uintptr_t handle = ReadPtr(component + 0x18);
        std::uintptr_t managed = handle ? ReadPtr(handle & ~7ull) : 0;
        if (!managed) managed = ReadPtr(component + 0x28);
        if (!managed) continue;
        if (IsClass(managed, className)) return managed;
    }
    return 0;
}

static std::uintptr_t ResolveGameViaGom()
{
    std::uintptr_t entryObject = FindGameObject("UnityEntry");
    if (!entryObject) entryObject = FindGameObject("[Entry]");
    if (!entryObject) return 0;

    const std::uintptr_t unityEntry = GetComponent(entryObject, "UnityEntry");
    if (!unityEntry) return 0;

    const std::uintptr_t game = ReadPtr(unityEntry + 0x28);
    return IsClass(game, "Game") ? game : 0;
}

// --- Verified root chain -------------------------------------------------
static std::uintptr_t ResolveBattleModel(std::uintptr_t game)
{
    if (game > 0x10000 && IsClass(game, "Game")) {
        const std::uintptr_t gameModel = ReadPtr(game + rr::Game::GameModel);
        if (IsClass(gameModel, "GameModel")) {
            const std::uintptr_t battleModel = ReadPtr(gameModel + rr::GameModel::BattleModel);
            if (IsClass(battleModel, "BattleModel"))
                return battleModel;
        }
    }

    // Fallback singleton. Still validated by class identity.
    const std::uintptr_t typeInfo = ReadPtr(gaBase + rr::TypeInfo::BattleModel);
    if (typeInfo > 0x10000) {
        const std::uintptr_t statics = ReadPtr(typeInfo + rr::Klass::static_fields);
        if (statics > 0x10000) {
            const std::uintptr_t battleModel = ReadPtr(statics);
            if (IsClass(battleModel, "BattleModel"))
                return battleModel;
        }
    }

    // AOB-backed fallback: recover the class table, find the class by name and
    // read its static instance. This does not depend on the TypeInfo RVAs.
    if (ClassFallbackDue()) {
        const std::uintptr_t battleModel = ResolveStaticSingleton(
            "BattleModel", "BattleModel");
        if (battleModel)
            return battleModel;
    }
    return 0;
}

static std::uintptr_t FindUpdater(std::uintptr_t game, std::uintptr_t battleModel)
{
    if (game > 0x10000) {
        const std::uintptr_t list = ReadPtr(game + rr::Game::ActiveUpdaters);
        const std::uintptr_t array = ReadPtr(list + rr::List::items);
        const std::int32_t count = RobRunMemory::ReadInt32(list + rr::List::count);
        if (array > 0x10000 && count > 0 && count <= 512) {
            for (int i = 0; i < count; ++i) {
                const std::uintptr_t a = ReadPtr(array + rr::Array::vector +
                    static_cast<std::uintptr_t>(i) * rr::Array::stride);
                const std::uintptr_t b = ReadPtr(a + rr::UpdaterChain::a_step);
                const std::uintptr_t c = ReadPtr(b + rr::UpdaterChain::b_step);
                const std::uintptr_t updater = ReadPtr(c + rr::UpdaterChain::c_step);
                if (!IsClass(updater, "BattleEntitiesUpdater")) continue;
                if (ReadPtr(updater + rr::BattleEntitiesUpdater::_battleModel) != battleModel)
                    continue;
                return updater;
            }
        }
    }

    const std::uintptr_t typeInfo = ReadPtr(gaBase + rr::TypeInfo::BattleEntitiesUpdater);
    if (typeInfo > 0x10000) {
        const std::uintptr_t statics = ReadPtr(typeInfo + rr::Klass::static_fields);
        if (statics > 0x10000) {
            const std::uintptr_t updater = ReadPtr(statics);
            if (IsClass(updater, "BattleEntitiesUpdater") &&
                ReadPtr(updater + rr::BattleEntitiesUpdater::_battleModel) == battleModel) {
                return updater;
            }
        }
    }

    // AOB-backed fallback, same contract as ResolveBattleModel.
    if (ClassFallbackDue()) {
        const std::uintptr_t updater = ResolveStaticSingleton(
            "BattleEntitiesUpdater", "BattleEntitiesUpdater");
        if (updater && ReadPtr(updater + rr::BattleEntitiesUpdater::_battleModel) == battleModel)
            return updater;
    }
    return 0;
}

static void ReadHealth(std::uintptr_t characterModel, EntityData& entity)
{
    if (characterModel <= 0x10000) return;
    const std::uintptr_t privateModel = ReadPtr(
        characterModel + rr::BattleCharacterModel::PrivateModel);
    entity.privateModel = privateModel;
    if (privateModel <= 0x10000) return;

    const std::uintptr_t healthModel = ReadPtr(
        privateModel + rr::BattleCharacterPrivateModel::HealthModel);
    if (healthModel <= 0x10000) return;

    struct HealthDataBlock {
        std::uintptr_t healthWrapper;
        std::uintptr_t isDeadWrapper;
        std::int32_t maxAmount;
        std::int32_t criticalHealth;
    } healthBlock{};
    if (RobRunMemory::ReadBytes(healthModel + rr::HealthModel::Health, &healthBlock, sizeof(healthBlock))) {
        float health = 0.f;
        if (healthBlock.healthWrapper > 0x10000 &&
            Il2Cpp::ReadPrimitiveFloat(healthBlock.healthWrapper, health) &&
            std::isfinite(health) && health >= 0.f && health <= 100000.f) {
            entity.health = health;
            entity.healthKnown = true;
        }
        if (healthBlock.maxAmount > 0 && healthBlock.maxAmount <= 100000)
            entity.maxHealth = static_cast<float>(healthBlock.maxAmount);
        if (healthBlock.criticalHealth >= 0 && healthBlock.criticalHealth <= 100000)
            entity.criticalHealth = static_cast<float>(healthBlock.criticalHealth);
    }

    const std::uintptr_t staminaModel = ReadPtr(
        privateModel + rr::BattleCharacterPrivateModel::Stamina);
    if (staminaModel > 0x10000) {
        const std::uintptr_t staminaAmountWrapper = ReadPtr(
            staminaModel + rr::StaminaModel::Amount);
        int staminaVal = 0;
        if (staminaAmountWrapper > 0x10000 &&
            Il2Cpp::ReadPrimitiveInt(staminaAmountWrapper, staminaVal)) {
            entity.stamina = static_cast<float>(staminaVal);
        }
    }

    const std::uintptr_t initArmorModel = ReadPtr(
        privateModel + rr::BattleCharacterPrivateModel::InitArmorModel);
    if (initArmorModel > 0x10000) {
        const std::uintptr_t armorAmountWrapper = ReadPtr(
            initArmorModel + rr::InitArmorModel::Amount);
        int armorVal = 0;
        if (armorAmountWrapper > 0x10000 &&
            Il2Cpp::ReadPrimitiveInt(armorAmountWrapper, armorVal)) {
            entity.armor = static_cast<float>(armorVal);
            if (armorVal > 0) entity.hasArmor = true;
        }
    }
}

static std::string NormalizeRoleName(const std::string& raw)
{
    if (raw.empty()) return {};
    std::string lower;
    lower.reserve(raw.size());
    for (unsigned char ch : raw) {
        if (ch < 0x20 || ch > 0x7E) continue;
        lower.push_back(static_cast<char>(std::tolower(ch)));
    }
    if (lower.empty()) return {};

    // Police first — "robbery" contains "rob" which would false-positive as Criminal
    if (lower.find("polic") != std::string::npos ||
        lower.find("cop") != std::string::npos ||
        lower.find("officer") != std::string::npos ||
        lower.find("swat") != std::string::npos ||
        lower.find("guard") != std::string::npos ||
        lower.find("sheriff") != std::string::npos)
        return "Police Officer";
    if (lower.find("crim") != std::string::npos ||
        lower.find("robber") != std::string::npos ||
        lower.find("thief") != std::string::npos ||
        lower.find("bandit") != std::string::npos ||
        lower.find("gang") != std::string::npos)
        return "Criminal";
    if (lower.find("civil") != std::string::npos ||
        lower.find("citizen") != std::string::npos ||
        lower.find("settler") != std::string::npos ||
        lower.find("bystander") != std::string::npos ||
        lower.find("npc") != std::string::npos)
        return "Civilian";
    if (lower.find("hub") != std::string::npos)
        return "Hubber";

    std::string clean;
    for (unsigned char ch : raw) {
        if (ch >= 0x20 && ch <= 0x7E) clean.push_back(static_cast<char>(ch));
    }
    return clean;
}

static bool ResolveRoleFromObject(std::uintptr_t obj, int& outRoleId,
    std::string& outRoleName, int depth = 0)
{
    if (depth >= 4 || obj <= 0x10000 || obj >= 0x00007FFFFFFFFFFFull)
        return false;

    const std::uintptr_t klass = Il2Cpp::ClassOf(obj);
    if (!klass) return false;
    const std::string clsName = Il2Cpp::ClassNameOfClass(klass);
    if (clsName.empty()) return false;

    if (clsName == "String") {
        std::wstring ws;
        if (RobRunMemory::ReadString(obj, ws, 128) && !ws.empty()) {
            const std::string norm = NormalizeRoleName(RobRunMemory::WideToUtf8(ws));
            if (!norm.empty()) {
                outRoleName = norm;
                return true;
            }
        }
        return false;
    }

    auto readIl2CppStringField = [&](std::uintptr_t offset, std::string& dst) -> bool {
        const std::uintptr_t strObj = ReadPtr(obj + offset);
        if (strObj <= 0x10000 || strObj >= 0x00007FFFFFFFFFFFull) return false;
        if (!Il2Cpp::IsClass(strObj, "String")) return false;
        std::wstring ws;
        if (!RobRunMemory::ReadString(strObj, ws, 128) || ws.empty()) return false;
        const std::string norm = NormalizeRoleName(RobRunMemory::WideToUtf8(ws));
        if (norm.empty()) return false;
        dst = norm;
        return true;
    };

    const auto fields = Il2Cpp::Fields(klass);
    if (!fields.empty()) {
        auto fieldMatches = [](const std::string& name, std::initializer_list<const char*> keys) {
            std::string low;
            low.reserve(name.size());
            for (unsigned char ch : name) low.push_back(static_cast<char>(std::tolower(ch)));
            for (const char* k : keys) {
                if (low.find(k) != std::string::npos) return true;
            }
            return false;
        };

        for (const auto& f : fields) {
            if (f.offset <= 0 || f.offset >= 0x400) continue;
            if (f.typeCode == 0x0E ||
                ((f.typeCode == 0x12 || f.typeCode == 0x1C) &&
                 fieldMatches(f.name, {"name", "id", "title", "role"}))) {
                if (readIl2CppStringField(static_cast<std::uintptr_t>(f.offset), outRoleName))
                    return true;
            }
        }

        for (const auto& f : fields) {
            if (f.offset <= 0 || f.offset >= 0x400) continue;
            if (f.typeCode == 0x12 || f.typeCode == 0x15 || f.typeCode == 0x1C) {
                if (fieldMatches(f.name, {"value", "data", "desc", "role"})) {
                    const std::uintptr_t child = ReadPtr(obj + static_cast<std::uintptr_t>(f.offset));
                    if (ResolveRoleFromObject(child, outRoleId, outRoleName, depth + 1))
                        return true;
                }
            }
        }

        for (const auto& f : fields) {
            if (f.offset <= 0 || f.offset >= 0x400) continue;
            if (readIl2CppStringField(static_cast<std::uintptr_t>(f.offset), outRoleName))
                return true;
        }

        for (const auto& f : fields) {
            if (f.offset <= 0 || f.offset >= 0x400) continue;
            if (f.typeCode == 0x08 || f.typeCode == 0x09 || f.typeCode == 0x11) {
                if (fieldMatches(f.name, {"value", "role", "type", "team", "index", "id"})) {
                    const std::uintptr_t rawPtr = ReadPtr(obj + static_cast<std::uintptr_t>(f.offset));
                    if (rawPtr > 0x10000 && rawPtr < 0x00007FFFFFFFFFFFull &&
                        Il2Cpp::ClassOf(rawPtr) != 0)
                        continue;
                    const std::int32_t v = RobRunMemory::ReadInt32(obj + static_cast<std::uintptr_t>(f.offset));
                    if (v >= 0 && v < 100000) {
                        outRoleId = v;
                        return true;
                    }
                }
            }
        }
    }

    const std::uintptr_t inner = ReadPtr(obj + rr::ReferenceData::Value);
    if (inner > 0x10000 && inner < 0x00007FFFFFFFFFFFull && Il2Cpp::ClassOf(inner) != 0) {
        if (ResolveRoleFromObject(inner, outRoleId, outRoleName, depth + 1))
            return true;
    }
    if (readIl2CppStringField(rr::Description::Name, outRoleName))
        return true;

    return false;
}

static void ApplyRoleFromWrapper(std::uintptr_t roleWrapper, EntityData& entity)
{
    if (roleWrapper <= 0x10000) return;

    // Never cache by roleWrapper itself: the wrapper object stays at the same
    // address while its inner RoleDescription* (+0x18) mutates live when a
    // Civilian becomes a Criminal. Read the inner value fresh and cache only
    // by the immutable RoleDescription* singleton pointer.
    std::uintptr_t innerPtr = 0;
    if (!TargetProcess.ReadFresh(roleWrapper + rr::ReferenceData::Value, &innerPtr, sizeof(innerPtr)))
        innerPtr = ReadPtr(roleWrapper + rr::ReferenceData::Value);

    if (innerPtr > 0x10000 && innerPtr < 0x7FFFFFFFFFFFull) {
        const auto it = g_roleCache.find(innerPtr);
        if (it != g_roleCache.end() && it->second.resolved) {
            if (!it->second.roleName.empty()) entity.roleName = it->second.roleName;
            if (it->second.roleId != 0) entity.role = it->second.roleId;
            return;
        }

        int roleId = 0;
        std::string roleName;
        if (ResolveRoleFromObject(innerPtr, roleId, roleName)) {
            if (!roleName.empty()) entity.roleName = roleName;
            if (roleId != 0) entity.role = roleId;
            if (!entity.roleName.empty()) {
                g_roleCache[innerPtr] = RoleCacheEntry{ roleId, roleName, true };
                return;
            }
        }

        // Direct fallback: RoleDescription + 0x10 -> Name string
        const std::uintptr_t nameStr = ReadPtr(innerPtr + rr::Description::Name);
        if (nameStr > 0x10000) {
            std::wstring w;
            if (RobRunMemory::ReadString(nameStr, w, 64) && !w.empty()) {
                const std::string norm = NormalizeRoleName(RobRunMemory::WideToUtf8(w));
                if (!norm.empty()) {
                    entity.roleName = norm;
                    g_roleCache[innerPtr] = RoleCacheEntry{ roleId, norm, true };
                    return;
                }
            }
        }

        if (Il2Cpp::ClassOf(innerPtr) != 0) {
            if (roleId != 0 || !roleName.empty())
                g_roleCache[innerPtr] = RoleCacheEntry{ roleId, roleName, true };
            return;
        }
    }

    // Fallback for wrappers that store a primitive int32 role (never cached by wrapper).
    int roleId = 0;
    std::string roleName;
    if (ResolveRoleFromObject(roleWrapper, roleId, roleName)) {
        if (!roleName.empty()) entity.roleName = roleName;
        if (roleId != 0) entity.role = roleId;
        if (!entity.roleName.empty())
            return;
    }

    std::int32_t role = 0;
    if (Il2Cpp::ReadPrimitiveInt(roleWrapper, role)) {
        if (role == 1257638368) {
            entity.role = role;
            entity.roleName = "Criminal";
        } else if (role > 0 && role < 100000) {
            entity.role = role;
        }
    }
}

static void ReadRoleAndVehicle(std::uintptr_t characterModel, EntityData& entity)
{
    if (characterModel <= 0x10000) return;

    const std::uintptr_t roleWrapper = ReadPtr(
        characterModel + rr::BattleCharacterModel::PlayerRole);
    ApplyRoleFromWrapper(roleWrapper, entity);

    const std::uintptr_t inVehicle = ReadPtr(
        characterModel + rr::BattleCharacterModel::InVehicle);
    if (inVehicle <= 0x10000) return;

    const std::uintptr_t vehicleWrapper = ReadPtr(
        inVehicle + rr::InVehicleModel::EntityId);
    std::int32_t vehicleId = 0;
    if (Il2Cpp::ReadPrimitiveInt(vehicleWrapper, vehicleId)) {
        entity.inVehicle = true;
        entity.vehicleId = vehicleId;
    }
}

// New offset set: BattleCharacterModel+0x40 -> Data+0x20 -> CommonData.
// One traversal covers role, movement state, death, firing and vehicle id.
static void ReadCommonState(std::uintptr_t characterModel, EntityData& entity)
{
    if (characterModel <= 0x10000) return;
    const std::uintptr_t data = ReadPtr(
        characterModel + rr::BattleCharacterModel::Data);
    if (data <= 0x10000) return;
    const std::uintptr_t common = ReadPtr(data + rr::BattleData::Common);
    if (common <= 0x10000) return;

    if (entity.roleName.empty() && entity.role == 0) {
        const std::uintptr_t roleW = ReadPtr(common + rr::CommonModel::Role);
        ApplyRoleFromWrapper(roleW, entity);
        // Direct offset fallback: role+0x18 → desc+0x10 → name string
        if (entity.roleName.empty() && entity.role == 0 && roleW > 0x10000) {
            const std::uintptr_t desc = ReadPtr(roleW + rr::ReferenceData::Value);
            if (desc > 0x10000 && desc < 0x7FFFFFFFFFFFull) {
                const std::uintptr_t nameStr = ReadPtr(desc + rr::Description::Name);
                if (nameStr > 0x10000) {
                    std::wstring w;
                    if (RobRunMemory::ReadString(nameStr, w, 64) && !w.empty()) {
                        const std::string norm = NormalizeRoleName(RobRunMemory::WideToUtf8(w));
                        if (!norm.empty()) entity.roleName = norm;
                    }
                }
            }
        }
    }

    bool flag = false;
    if (Il2Cpp::ReadPrimitiveBool(ReadPtr(common + rr::CommonModel::IsFiring), flag))
        entity.isFiring = flag;
    if (Il2Cpp::ReadPrimitiveBool(ReadPtr(common + rr::CommonModel::IsSprinting), flag))
        entity.isSprinting = flag;
    if (Il2Cpp::ReadPrimitiveBool(ReadPtr(common + rr::CommonModel::IsCrouching), flag))
        entity.isCrouching = flag;
    if (Il2Cpp::ReadPrimitiveBool(ReadPtr(common + rr::CommonModel::IsDead), flag))
        entity.isDead = flag;
    if (Il2Cpp::ReadPrimitiveBool(
            ReadPtr(characterModel + rr::BattleCharacterModel::HasArmor), flag))
        entity.hasArmor = flag;
    // PublicData hasArmor fallback: Data+0x30 → +0x38 → +0x10
    if (!entity.hasArmor) {
        const std::uintptr_t pub = ReadPtr(data + 0x30);
        if (pub > 0x10000) {
            const std::uintptr_t armorW = ReadPtr(pub + 0x38);
            if (armorW > 0x10000) {
                const std::uint8_t val = RobRunMemory::ReadUInt8(armorW + 0x10);
                if (val != 0) entity.hasArmor = true;
            }
        }
    }

    std::int32_t vehicleId = 0;
    if (Il2Cpp::ReadPrimitiveInt(ReadPtr(common + rr::CommonModel::VehicleEntityId), vehicleId)) {
        entity.inVehicle = vehicleId > 0;
        entity.vehicleId = vehicleId > 0 ? vehicleId : 0;
    }
}

// Current battle clock in Unix milliseconds. Reload/knockdown end timestamps
// are compared against this instead of the wall clock: the server clock can
// drift, and a timestamp that is already in the past must not read as active.
static std::int64_t ReadBattleNowMs(std::uintptr_t battleEntities)
{
    if (battleEntities > 0x10000) {
        const std::uintptr_t localPlayer = ReadPtr(
            battleEntities + rr::BattleEntitiesModel::LocalPlayerEntityModel);
        const std::uintptr_t client = localPlayer > 0x10000
            ? ReadPtr(localPlayer + rr::PlayerEntityModel::Client)
            : 0;
        const std::uintptr_t intention = client > 0x10000
            ? ReadPtr(client + rr::PlayerClient::InventoryIntention)
            : 0;
        const std::uintptr_t nowObj = intention > 0x10000
            ? ReadPtr(intention + rr::InventoryIntentionModel::Now)
            : 0;
        if (nowObj > 0x10000) {
            const std::int64_t t = RobRunMemory::ReadInt64(
                nowObj + rr::InventoryIntentionModel::TimeMs);
            if (t > 1000000000000LL) return t;
        }
    }
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    const std::uint64_t t100ns =
        (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    constexpr std::uint64_t epochDiff = 116444736000000000ULL;
    return (t100ns > epochDiff)
        ? static_cast<std::int64_t>((t100ns - epochDiff) / 10000ULL) : 0;
}

// Reference wrapper -> ReferenceData -> value, then the timestamp payload.
static std::int64_t ReadTimestampData(std::uintptr_t tsWrapper)
{
    if (tsWrapper <= 0x10000) return 0;
    return RobRunMemory::ReadInt64(tsWrapper + 0x18);
}

static constexpr std::int64_t kMaxKnockdownRemainMs = 60000LL;
static constexpr std::int64_t kMaxKnockoutRemainMs  = 300000LL;

// Validates a KnockoutModel::Phase ReferenceData::Value pointer.
// Rejects non-canonical garbage and non-knockout/idle/default/cuffing phase descriptions.
static bool IsActiveKnockoutPhaseDesc(std::uintptr_t phaseDesc)
{
    if (phaseDesc <= 0x10000 || phaseDesc >= 0x00007FFFFFFFFFFFull)
        return false;
    static std::unordered_map<std::uintptr_t, bool> s_phaseValidCache;
    if (s_phaseValidCache.size() > 256)
        s_phaseValidCache.clear();
    const auto it = s_phaseValidCache.find(phaseDesc);
    if (it != s_phaseValidCache.end())
        return it->second;

    bool valid = true;
    const std::uintptr_t nameStr = ReadPtr(phaseDesc + rr::Description::Name);
    if (nameStr > 0x10000 && nameStr < 0x00007FFFFFFFFFFFull) {
        std::wstring w;
        if (RobRunMemory::ReadString(nameStr, w, 64)) {
            std::string lower = RobRunMemory::WideToUtf8(w);
            for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lower.empty() ||
                lower.find("none") != std::string::npos ||
                lower.find("idle") != std::string::npos ||
                lower.find("normal") != std::string::npos ||
                lower.find("default") != std::string::npos ||
                lower.find("stand") != std::string::npos ||
                lower.find("cuffing") != std::string::npos) {
                valid = false;
            }
        } else {
            valid = false;
        }
    } else {
        // Not a real Description object with a valid name string
        valid = false;
    }
    s_phaseValidCache[phaseDesc] = valid;
    return valid;
}

static void ReadReloadAndKnockdownState(std::uintptr_t characterModel,
    std::int64_t battleNowMs, EntityData& entity)
{
    if (characterModel <= 0x10000) return;
    constexpr std::int64_t kSentinelTs = 1767225600000LL;

    // 1) Reload state via WeaponReloadModel._state (ReferenceData -> +0x18
    //    non-null while reloading).
    const std::uintptr_t reloadModel = ReadPtr(
        characterModel + rr::BattleCharacterModel::WeaponReload);
    if (reloadModel > 0x10000) {
        const std::uintptr_t stateRef = ReadPtr(
            reloadModel + rr::WeaponReloadModel::State);
        const std::uintptr_t stateDesc = stateRef > 0x10000
            ? ReadPtr(stateRef + rr::ReferenceData::Value)
            : 0;
        if (stateDesc > 0x10000) {
            entity.isReloading = true;
            std::wstring w;
            RobRunMemory::ReadString(
                ReadPtr(stateDesc + rr::Description::Name), w);
            entity.reloadPhase = RobRunMemory::WideToUtf8(w);
        }
        if (!entity.isReloading && battleNowMs > 0) {
            const std::int64_t endTs = ReadTimestampData(
                ReadPtr(reloadModel + rr::WeaponReloadModel::EndTs));
            if (endTs > kSentinelTs && endTs > battleNowMs)
                entity.isReloading = true;
        }
    }
    if (!entity.isReloading) {
        bool bgReload = false;
        if (Il2Cpp::ReadPrimitiveBool(
                ReadPtr(characterModel + rr::BattleCharacterModel::IsBackgroundReload),
                bgReload) && bgReload) {
            entity.isReloading = true;
        }
    }

    // 2) KnockdownModel._endTs (active while after the sentinel and now),
    //    gated by KnockdownLeaveStateModel — once the leave phase starts
    //    the player is getting up and should no longer count as knocked.
    const std::uintptr_t kdModel = ReadPtr(
        characterModel + rr::BattleCharacterModel::Knockdown);
    if (kdModel > 0x10000 && battleNowMs > 0) {
        const std::int64_t kdEndTs = ReadTimestampData(
            ReadPtr(kdModel + rr::KnockdownModel::EndTs));
        if (kdEndTs > kSentinelTs && kdEndTs > battleNowMs) {
            const std::uintptr_t leaveModel = ReadPtr(
                characterModel + rr::BattleCharacterModel::KnockdownLeaveState);
            const bool leaving = leaveModel > 0x10000 &&
                RobRunMemory::ReadUInt8(leaveModel + rr::KnockdownLeaveStateModel::IsActive) != 0;
            if (!leaving)
                entity.isKnockedDown = true;
        }
    }

    // 3) KnockoutModel (phase non-null, active endTs, surrendered / handcuffed).
    const std::uintptr_t koModel = ReadPtr(
        characterModel + rr::BattleCharacterModel::Knockout);
    if (koModel > 0x10000) {
        const std::uintptr_t phaseRef = ReadPtr(
            koModel + rr::KnockoutModel::Phase);
        const std::uintptr_t phaseDesc = phaseRef > 0x10000
            ? ReadPtr(phaseRef + rr::ReferenceData::Value)
            : 0;
        if (phaseDesc > 0x10000) {
            entity.isKnockedOut = true;
        } else if (battleNowMs > 0) {
            const std::int64_t koEndTs = ReadTimestampData(
                ReadPtr(koModel + rr::KnockoutModel::EndTs));
            if (koEndTs > kSentinelTs && koEndTs > battleNowMs)
                entity.isKnockedOut = true;
        }
        entity.isSurrendered =
            RobRunMemory::ReadUInt8(koModel + rr::KnockoutModel::IsSurrendered) != 0;
        entity.isHandcuffed =
            RobRunMemory::ReadUInt8(koModel + rr::KnockoutModel::IsHandcuffed) != 0;
    }
}

static std::wstring CleanWeaponName(const std::wstring& name)
{
    static constexpr std::pair<std::wstring_view, std::wstring_view> table[] = {
        { L"kl",        L"AK"           },
        { L"srreaper",  L"AS VAL"       },
        { L"jp7",       L"MP7"          },
        { L"mc10",      L"Mac10"        },
        { L"harris500", L"Pump Shotgun" },
        { L"cb45",      L"UMP45"        },
        { L"dp9",       L"Pistol"       },
        { L"ordr12",    L"SPAZ-12"      },
        { L"tag12k",    L"TAG-12K"      },
        { L"lbr44",     L"Revolver"     },
        { L"rc556",     L"R4C"          },
        { L"vulture50", L"Deagle"       },
        { L"apscalar",  L"Vector"       },
        { L"starh",     L"Scar-H"      },
        { L"pdw90",     L"P90"          },
    };

    const size_t pos = name.find(L'_');
    const std::wstring_view weapName = std::wstring_view(name).substr(0, pos);
    const std::wstring_view weapTier = (pos != std::wstring::npos)
        ? std::wstring_view(name).substr(pos + 1) : std::wstring_view{};

    for (const auto& [key, clean] : table) {
        if (weapName.find(key) != std::wstring_view::npos) {
            std::wstring result(clean);
            if (!weapTier.empty()) {
                result += L" | MK";
                result += weapTier.substr(1);
            }
            return result;
        }
    }
    return {};
}

static void ReadWeaponName(std::uintptr_t characterModel, std::wstring& out, int& weaponIndex)
{
    out.clear();
    weaponIndex = -1;
    if (characterModel <= 0x10000) return;
    const std::uintptr_t itemInHands = ReadPtr(
        characterModel + rr::BattleCharacterModel::ItemInHands);
    if (itemInHands <= 0x10000) return;
    const std::uintptr_t current = ReadPtr(
        itemInHands + rr::ItemInHandsModel::Current);
    if (current <= 0x10000) return;
    const std::uintptr_t data = ReadPtr(
        current + rr::EquipItemModel::Data);
    if (data <= 0x10000) return;
    const std::uintptr_t description = ReadPtr(data + rr::ReferenceData::Value);
    if (description <= 0x10000) return;

    const auto it = g_weaponDescCache.find(description);
    if (it != g_weaponDescCache.end() && it->second.valid) {
        out = it->second.name;
        weaponIndex = it->second.index;
        return;
    }

    const std::uintptr_t nameStr = ReadPtr(
        description + rr::Description::Name);
    if (nameStr > 0x10000)
        RobRunMemory::ReadString(nameStr, out);
    if (!out.empty()) {
        std::wstring cleaned = CleanWeaponName(out);
        if (!cleaned.empty()) out = std::move(cleaned);
    }
    const std::int32_t idx = RobRunMemory::ReadInt32(
        description + rr::InventoryItemDescription::Index);
    if (idx >= 0) weaponIndex = idx;

    if (!out.empty() || weaponIndex >= 0) {
        g_weaponDescCache[description] = WeaponDescCache{ out, weaponIndex, true };
    }
}

// Read a slot identity string. The new offset set stores Il2CppString*
// directly on BattleCharacterSlotData (+0x30 / +0x38); older sessions put a
// reference wrapper at that field whose value is +0x18. Validate the runtime
// class so a wrapper cannot be mistaken for a string.
static bool ReadSlotString(std::uintptr_t owner, std::uintptr_t field, std::wstring& out)
{
    out.clear();
    if (owner <= 0x10000) return false;

    const std::uintptr_t value = ReadPtr(owner + field);
    if (value <= 0x10000) return false;

    if (RobRunMemory::ReadString(value, out) && !out.empty())
        return true;

    const std::uintptr_t wrapped = ReadPtr(value + rr::BattleCharacterSlotData::wrapper_value);
    if (wrapped > 0x10000 && wrapped != value &&
        RobRunMemory::ReadString(wrapped, out) && !out.empty())
        return true;

    out.clear();
    return false;
}

static void ReadSlotIdentity(std::uintptr_t slotValue, EntityData& entity)
{
    if (slotValue <= 0x10000) return;

    const auto it = g_slotIdentityCache.find(slotValue);
    if (it != g_slotIdentityCache.end() && it->second.valid) {
        entity.nickname = it->second.nickname;
        entity.clanTag = it->second.clanTag;
        entity.publicId = it->second.publicId;
        return;
    }

    std::uintptr_t slotData = slotValue;
    const std::uintptr_t dataCandidate = ReadPtr(slotValue + rr::BattleCharacterSlotModel::_data);
    if (dataCandidate > 0x10000 && dataCandidate != slotValue) {
        slotData = dataCandidate;
    }

    ReadSlotString(slotData, rr::BattleCharacterSlotData::Nickname, entity.nickname);
    ReadSlotString(slotData, rr::BattleCharacterSlotData::ClanTag, entity.clanTag);

    const std::uintptr_t pubWrap = ReadPtr(slotData + rr::BattleCharacterSlotData::PublicId);
    if (pubWrap > 0x10000) {
        const std::int64_t val = RobRunMemory::ReadInt64(pubWrap + 0x18);
        if (val > 0)
            entity.publicId = static_cast<std::uint64_t>(val);
        else {
            const std::int64_t raw = RobRunMemory::ReadInt64(slotData + rr::BattleCharacterSlotData::PublicId);
            if (raw > 0) entity.publicId = static_cast<std::uint64_t>(raw);
        }
    } else {
        const std::int64_t raw = RobRunMemory::ReadInt64(slotData + rr::BattleCharacterSlotData::PublicId);
        if (raw > 0) entity.publicId = static_cast<std::uint64_t>(raw);
    }

    if (entity.publicId > 0 || !entity.nickname.empty()) {
        g_slotIdentityCache[slotValue] = SlotIdentityCache{
            entity.nickname, entity.clanTag, entity.publicId, true
        };
        if (entity.publicId > 0 && !entity.nickname.empty()) {
            g_publicIdToNick[entity.publicId] = RobRunMemory::WideToUtf8(entity.nickname);
        }
    }
}

// --- Party membership -----------------------------------------------------
// The battle party data stores the local party's member list. Two updaters
// expose it: BattlePartyUpdater (battle party data with a Members list) and
// BattlePartyMemberModelsUpdater (the party model / member models used for
// positions). Both are resolved by class name, then their fields are matched
// by name, so a metadata shift does not require new offsets. A read only
// counts when the local player appears in it; anything else leaves the filter
// disabled rather than skipping strangers.

static bool FindFieldNamed(std::uintptr_t klass, const char* const* names,
    std::size_t count, Il2Cpp::Field& out)
{
    if (klass <= 0x10000) return false;
    const auto fields = Il2Cpp::Fields(klass, true);
    for (const auto& field : fields) {
        if (field.offset <= 0 || field.offset > 0x2000) continue;
        for (std::size_t i = 0; i < count; ++i) {
            if (field.name == names[i]) { out = field; return true; }
        }
    }
    return false;
}

static bool FindFieldContaining(std::uintptr_t klass, const char* needle,
    Il2Cpp::Field& out)
{
    if (klass <= 0x10000 || !needle || !*needle) return false;
    const auto fields = Il2Cpp::Fields(klass, true);
    for (const auto& field : fields) {
        if (field.offset <= 0 || field.offset > 0x2000) continue;
        if (field.name.find(needle) != std::string::npos) { out = field; return true; }
    }
    return false;
}

static bool ReadObjectCollection(std::uintptr_t object,
    std::vector<std::uintptr_t>& out)
{
    out.clear();
    if (object <= 0x10000) return false;
    const std::string klass = Il2Cpp::ClassNameOf(object);
    if (klass.find("List") != std::string::npos) {
        const std::uintptr_t items = ReadPtr(object + rr::List::items);
        const std::int32_t count = RobRunMemory::ReadInt32(object + rr::List::count);
        if (items <= 0x10000 || count <= 0 || count > 64) return false;
        out.reserve(static_cast<std::size_t>(count));
        for (std::int32_t i = 0; i < count; ++i) {
            out.push_back(ReadPtr(items + rr::Array::vector +
                static_cast<std::uintptr_t>(i) * rr::Array::stride));
        }
        return !out.empty();
    }
    if (klass.find("[]") != std::string::npos) {
        const std::int64_t length = RobRunMemory::ReadInt64(object + rr::Array::max_length);
        if (length <= 0 || length > 64) return false;
        out.reserve(static_cast<std::size_t>(length));
        for (std::int64_t i = 0; i < length; ++i) {
            out.push_back(ReadPtr(object + rr::Array::vector +
                static_cast<std::uintptr_t>(i) * rr::Array::stride));
        }
        return !out.empty();
    }
    return false;
}

static bool ReadLongCollection(std::uintptr_t object,
    std::vector<std::uint64_t>& out)
{
    out.clear();
    if (object <= 0x10000) return false;
    const std::string klass = Il2Cpp::ClassNameOf(object);
    const std::uintptr_t type = Il2Cpp::ClassOf(object);

    if (klass.find("HashSet") != std::string::npos) {
        Il2Cpp::Field entries{}, count{};
        if (!FindFieldContaining(type, "entries", entries)) return false;
        if (!FindFieldContaining(type, "count", count)) return false;
        const std::uintptr_t array = ReadPtr(object + entries.offset);
        const std::int32_t size = RobRunMemory::ReadInt32(object + count.offset);
        if (array <= 0x10000 || size <= 0 || size > 64) return false;
        // Entry<T> is { int hash; int next; T value; }. Public ids are 64-bit,
        // so the entry stride is 16 and the value sits at +8.
        for (std::int32_t i = 0; i < size; ++i) {
            const std::int64_t value = RobRunMemory::ReadInt64(
                array + rr::Array::vector + static_cast<std::uintptr_t>(i) * 16 + 8);
            if (value > 0) out.push_back(static_cast<std::uint64_t>(value));
        }
        return !out.empty();
    }

    if (klass.find("List") != std::string::npos) {
        const std::uintptr_t items = ReadPtr(object + rr::List::items);
        const std::int32_t count = RobRunMemory::ReadInt32(object + rr::List::count);
        if (items <= 0x10000 || count <= 0 || count > 64) return false;
        for (std::int32_t i = 0; i < count; ++i) {
            const std::int64_t value = RobRunMemory::ReadInt64(
                items + rr::Array::vector + static_cast<std::uintptr_t>(i) * 8);
            if (value > 0) out.push_back(static_cast<std::uint64_t>(value));
        }
        return !out.empty();
    }

    if (klass.find("[]") != std::string::npos) {
        const std::int64_t length = RobRunMemory::ReadInt64(object + rr::Array::max_length);
        if (length <= 0 || length > 64) return false;
        for (std::int64_t i = 0; i < length; ++i) {
            const std::int64_t value = RobRunMemory::ReadInt64(
                object + rr::Array::vector + static_cast<std::uintptr_t>(i) * 8);
            if (value > 0) out.push_back(static_cast<std::uint64_t>(value));
        }
        return !out.empty();
    }
    return false;
}

static bool ReadDictionaryMembers(std::uintptr_t dictionary,
    std::vector<std::uint64_t>& ids, std::vector<std::uintptr_t>& objects)
{
    if (dictionary <= 0x10000) return false;
    const std::uintptr_t entries = ReadPtr(dictionary + rr::Dict::entries);
    const std::int32_t count = RobRunMemory::ReadInt32(dictionary + rr::Dict::count);
    if (entries <= 0x10000 || count <= 0 || count > 64) return false;
    bool any = false;
    for (std::int32_t i = 0; i < count; ++i) {
        const std::uintptr_t entry = entries + rr::Array::vector +
            static_cast<std::uintptr_t>(i) * rr::Dict::entry_stride;
        const std::int32_t next = RobRunMemory::ReadInt32(entry + rr::Dict::entry_next);
        if (next < rr::Dict::entry_allocated_min) continue;
        const std::int64_t key = RobRunMemory::ReadInt64(entry + rr::Dict::entry_key);
        if (key > 0) { ids.push_back(static_cast<std::uint64_t>(key)); any = true; }
        const std::uintptr_t value = ReadPtr(entry + rr::Dict::entry_value);
        if (value > 0x10000) { objects.push_back(value); any = true; }
    }
    return any;
}

static void CollectPartyMember(std::uintptr_t member, PartyMemberIds& ids)
{
    if (member <= 0x10000) return;
    const std::uintptr_t klass = Il2Cpp::ClassOf(member);
    const char* idNames[] = {
        "PublicId", "<PublicId>k__BackingField", "publicId", "_publicId" };
    Il2Cpp::Field field{};
    if (FindFieldNamed(klass, idNames, 4, field)) {
        const std::int64_t value = RobRunMemory::ReadInt64(member + field.offset);
        if (value > 0) ids.publicIds.push_back(static_cast<std::uint64_t>(value));
    }
    const char* entityNames[] = {
        "EntityId", "<EntityId>k__BackingField", "entityId", "_entityId" };
    if (FindFieldNamed(klass, entityNames, 4, field)) {
        const std::int32_t value = RobRunMemory::ReadInt32(member + field.offset);
        if (value > 0) ids.entityIds.push_back(value);
    }
}

static bool ReadPartyDataObject(std::uintptr_t data, PartyMemberIds& ids)
{
    if (data <= 0x10000) return false;
    const std::uintptr_t klass = Il2Cpp::ClassOf(data);
    bool found = false;
    Il2Cpp::Field field{};

    const char* memberNames[] = {
        "Members", "<Members>k__BackingField", "_members", "members" };
    if (FindFieldNamed(klass, memberNames, 4, field)) {
        const std::uintptr_t collection = ReadPtr(data + field.offset);
        if (collection > 0x10000) {
            if (Il2Cpp::ClassNameOf(collection).find("Dictionary") != std::string::npos) {
                std::vector<std::uintptr_t> objects;
                found |= ReadDictionaryMembers(collection, ids.publicIds, objects);
                for (const auto object : objects) CollectPartyMember(object, ids);
            } else {
                std::vector<std::uintptr_t> objects;
                if (ReadObjectCollection(collection, objects)) {
                    for (const auto object : objects) CollectPartyMember(object, ids);
                    found = true;
                }
            }
        }
    }

    const char* idNames[] = {
        "MemberPublicIds", "<MemberPublicIds>k__BackingField",
        "_memberPublicIds", "memberPublicIds" };
    if (FindFieldNamed(klass, idNames, 4, field)) {
        const std::uintptr_t collection = ReadPtr(data + field.offset);
        if (ReadLongCollection(collection, ids.publicIds)) found = true;
    }

    const char* entityNames[] = {
        "Entities", "<Entities>k__BackingField", "_entities", "entities" };
    if (FindFieldNamed(klass, entityNames, 4, field)) {
        const std::uintptr_t collection = ReadPtr(data + field.offset);
        std::vector<std::uintptr_t> objects;
        if (ReadObjectCollection(collection, objects)) {
            for (const auto object : objects) CollectPartyMember(object, ids);
            found = true;
        }
    }

    return found;
}

static std::uintptr_t FindActiveUpdaterByName(std::uintptr_t game, const char* name)
{
    if (game <= 0x10000 || !name || !*name) return 0;
    const std::uintptr_t list = ReadPtr(game + rr::Game::ActiveUpdaters);
    const std::uintptr_t array = ReadPtr(list + rr::List::items);
    const std::int32_t count = RobRunMemory::ReadInt32(list + rr::List::count);
    if (array <= 0x10000 || count <= 0 || count > 512) return 0;
    for (std::int32_t i = 0; i < count; ++i) {
        const std::uintptr_t a = ReadPtr(array + rr::Array::vector +
            static_cast<std::uintptr_t>(i) * rr::Array::stride);
        const std::uintptr_t b = ReadPtr(a + rr::UpdaterChain::a_step);
        const std::uintptr_t c = ReadPtr(b + rr::UpdaterChain::b_step);
        const std::uintptr_t updater = ReadPtr(c + rr::UpdaterChain::c_step);
        if (IsClass(updater, name)) return updater;
        if (IsClass(a, name)) return a;
    }
    return 0;
}

static void RefreshParty(std::uintptr_t game, std::int32_t localId,
    std::uint64_t localPublicId)
{
    PartyMemberIds ids;
    bool any = false;

    // Battle party data carries the authoritative Members list.
    std::uintptr_t updater = FindActiveUpdaterByName(game, "BattlePartyUpdater");
    if (updater) {
        const char* names[] = {
            "_battlePartyData", "battlePartyData",
            "<BattlePartyData>k__BackingField" };
        Il2Cpp::Field field{};
        if (FindFieldNamed(Il2Cpp::ClassOf(updater), names, 3, field)) {
            const std::uintptr_t data = ReadPtr(updater + field.offset);
            if (ReadPartyDataObject(data, ids)) any = true;
        }
    }

    // Fallback: the member-models updater mirrors the local party model.
    if (!any) {
        updater = FindActiveUpdaterByName(game, "BattlePartyMemberModelsUpdater");
        if (updater) {
            const std::uintptr_t klass = Il2Cpp::ClassOf(updater);
            Il2Cpp::Field field{};
            const char* modelNames[] = {
                "_partyModel", "partyModel", "<PartyModel>k__BackingField" };
            if (FindFieldNamed(klass, modelNames, 3, field)) {
                const std::uintptr_t model = ReadPtr(updater + field.offset);
                if (ReadPartyDataObject(model, ids)) any = true;
            }
            if (!any) {
                const char* dataNames[] = {
                    "_partyMembersData", "partyMembersData" };
                if (FindFieldNamed(klass, dataNames, 2, field)) {
                    const std::uintptr_t data = ReadPtr(updater + field.offset);
                    if (ReadPartyDataObject(data, ids)) any = true;
                }
            }
        }
    }

    if (!any)
        ids = PartyMemberIds{};
    else
        ids.valid = true;
    g_party = std::move(ids);
}

static bool IsPartyMember(std::int32_t entityId, std::uint64_t publicId)
{
    if (!g_party.valid) return false;
    if (publicId > 0 &&
        std::find(g_party.publicIds.begin(), g_party.publicIds.end(), publicId) !=
            g_party.publicIds.end())
        return true;
    return std::find(g_party.entityIds.begin(), g_party.entityIds.end(), entityId) !=
        g_party.entityIds.end();
}

static int fingerprintFailures = 0;

static void RevalidateSession()
{
    if (!gaBase) return;

    // The PE fingerprint is the hard gate. Accessor signatures are advisory:
    // they surface unexpected toolchain changes without forcing repeated
    // re-resolution when a signature prefix differs.
    if (!Il2Cpp::VerifyFingerprint()) {
        // A single failed read of the PE header can be a DMA hiccup. Clearing
        // the whole session on the first miss forces a full re-resolve and
        // drops every cached rig, which shows up as a skeleton stutter.
        if (++fingerprintFailures < 2) return;
        DLOG("Fingerprint: sustained failure, resetting roots and metadata caches");
        ++failureCount;
        cachedGame = 0;
        cachedBattleModel = 0;
        cachedBattleEntities = 0;
        cachedCameraModel = 0;
        cachedLocalCharacter = 0;
        cachedLookDirectionModel = 0;
        cachedLookDirDirect = 0;
        cachedPlayerRotationInput = 0;
        cachedUpdater = 0;
        Il2Cpp::ResetCaches();
        fingerprintFailures = 0;
        return;
    }
    fingerprintFailures = 0;
    (void)Il2Cpp::VerifyAccessors();

    if (cachedGame && !IsClass(cachedGame, "Game")) {
        DLOG("GameRoot: class check failed for cachedGame=0x%llx", (unsigned long long)cachedGame);
        cachedGame = 0;
    }
    // Check live Game -> GameModel -> BattleModel with uncached DMA so leaving
    // a map or loading into a new session is caught immediately instead of
    // sticking to an old (not yet GC'd) BattleModel under -norefresh.
    if (cachedGame > 0x10000) {
        std::uintptr_t liveGameModel = 0;
        if (TargetProcess.ReadFresh(cachedGame + rr::Game::GameModel, &liveGameModel, sizeof(liveGameModel)) &&
            liveGameModel > 0x10000)
        {
            std::uintptr_t liveBattleModel = 0;
            if (TargetProcess.ReadFresh(liveGameModel + rr::GameModel::BattleModel, &liveBattleModel, sizeof(liveBattleModel))) {
                if (liveBattleModel != cachedBattleModel) {
                    DLOG("ReaderRoot: BattleModel changed 0x%llx -> 0x%llx, refreshing DMA caches",
                         (unsigned long long)cachedBattleModel, (unsigned long long)liveBattleModel);
                    MaybeRefreshDmaCaches("battleModelChanged", 0);
                    cachedBattleModel = (liveBattleModel > 0x10000 && IsClass(liveBattleModel, "BattleModel"))
                        ? liveBattleModel : 0;
                    cachedBattleEntities = 0;
                    cachedUpdater = 0;
                    cachedCameraModel = 0;
                    cachedLocalCharacter = 0;
                }
            }
        }
    }
    if (cachedBattleModel && !IsClass(cachedBattleModel, "BattleModel")) {
        DLOG("ReaderRoot: class check failed for BattleModel=0x%llx", (unsigned long long)cachedBattleModel);
        cachedBattleModel = 0;
    }
    if (cachedBattleEntities && !IsClass(cachedBattleEntities, "BattleEntitiesModel")) {
        DLOG("ReaderRoot: class check failed for BattleEntitiesModel=0x%llx", (unsigned long long)cachedBattleEntities);
        cachedBattleEntities = 0;
    }
    if (cachedUpdater && !IsClass(cachedUpdater, "BattleEntitiesUpdater")) {
        DLOG("ReaderRoot: class check failed for BattleEntitiesUpdater=0x%llx", (unsigned long long)cachedUpdater);
        cachedUpdater = 0;
    }
}

static bool HideDefaultCrosshair(std::uintptr_t game)
{
    if (g_crosshairState.hidden)
        return true;
    const std::uintptr_t updater = FindActiveUpdaterByName(game, "PlayerAimPointerUpdater");
    if (!updater || !IsClass(updater, "PlayerAimPointerUpdater"))
        return false;

    const std::uintptr_t original = ReadPtr(updater + 0xD8);
    const std::uintptr_t screenOpenChecker = ReadPtr(updater + 0xB8);
    if (original <= 0x10000 || !IsClass(screenOpenChecker, "ScreenOpenChecker"))
        return false;

    const std::uint32_t redirectFlag = RobRunMemory::ReadUInt32(screenOpenChecker + 0x14);
    if (redirectFlag == 0)
        return false;

    if (!TargetProcess.Write(updater + 0xD8, screenOpenChecker))
        return false;

    g_crosshairState.updater  = updater;
    g_crosshairState.original = original;
    g_crosshairState.hidden   = true;
    return true;
}

static bool RestoreDefaultCrosshair()
{
    if (!g_crosshairState.hidden)
        return true;
    if (g_crosshairState.updater <= 0x10000)
        return false;

    const bool restored = TargetProcess.Write(
        g_crosshairState.updater + 0xD8,
        g_crosshairState.original);

    if (restored)
        g_crosshairState = {};
    return restored;
}

static bool ReadRectTransformUiBounds(std::uintptr_t managedRect, RectTransformCache& cache, bool forceRefresh, const char* tag)
{
    if (managedRect <= 0x10000) {
        cache.valid = false;
        return false;
    }
    const std::uint64_t now = GetTickCount64();
    if (!forceRefresh && cache.managed == managedRect && (now - cache.stampMs) < 500)
        return cache.valid;

    cache.managed = managedRect;
    cache.stampMs = now;

    // Uncached-first read helpers: under -norefresh, newly instantiated UI
    // dialogs (e.g. MapDialogView on pressing 'M') may not be in VMM's cache.
    auto mapReadBytes = [](std::uintptr_t addr, void* buf, std::size_t sz) -> bool {
        if (addr <= 0x10000 || !buf || !sz) return false;
        if (TargetProcess.ReadFresh(addr, buf, sz)) return true;
        return TargetProcess.Read(addr, buf, sz);
    };
    auto mapReadPtr = [&](std::uintptr_t addr) -> std::uintptr_t {
        std::uintptr_t v = 0;
        return mapReadBytes(addr, &v, sizeof(v)) ? v : 0;
    };
    auto mapReadI32 = [&](std::uintptr_t addr, std::int32_t& out) -> bool {
        return mapReadBytes(addr, &out, sizeof(out));
    };

    const std::uintptr_t native = mapReadPtr(managedRect + rr::UnityTransform::native);
    if (native <= 0x10000) { cache.valid = false; return false; }
    const std::uintptr_t hierarchy = mapReadPtr(native + rr::UnityTransform::hierarchy);
    if (hierarchy <= 0x10000) { cache.valid = false; return false; }

    const std::uintptr_t matrices = mapReadPtr(hierarchy + rr::UnityTransform::matrices);
    const std::uintptr_t parents  = mapReadPtr(hierarchy + rr::UnityTransform::parents);
    const std::uintptr_t pointers = mapReadPtr(hierarchy + rr::UnityTransform::pointers);
    if (matrices <= 0x10000 || parents <= 0x10000) {
        cache.valid = false;
        return false;
    }

    // 1. Resolve transform index inside the hierarchy.
    // Fast path: Native Transform + 0x30 holds the uint32 transform index
    // (verified in single 8-byte read against hierarchy.pointers[idx] == native).
    int idx = -1;
    if (pointers > 0x10000) {
        constexpr std::uintptr_t kIdxOffsets[] = {
            rr::UnityTransform::index, 0x20, 0x34, 0x38, 0x40, 0x44, 0x48
        };
        for (std::uintptr_t off : kIdxOffsets) {
            std::int32_t cand = -1;
            if (mapReadI32(native + off, cand) && cand >= 0 && cand < 131072) {
                if (mapReadPtr(pointers + static_cast<std::uintptr_t>(cand) * sizeof(std::uintptr_t)) == native) {
                    idx = cand;
                    break;
                }
            }
        }
        // Fallback: chunked 4 KB scan of hierarchy.pointers (tolerates large
        // UI Canvas hierarchies with >4096 elements without multi-page cache misses).
        if (idx < 0) {
            std::int32_t count = 0;
            if (!mapReadI32(hierarchy + rr::UnityTransform::count, count) || count < 1 || count > 65536) {
                if (!mapReadI32(hierarchy + 0x10, count) || count < 1 || count > 65536)
                    count = 16384;
            }
            constexpr int kChunkPtrs = 512;
            std::uintptr_t chunk[kChunkPtrs];
            for (int base = 0; base < count && idx < 0; base += kChunkPtrs) {
                const int n = std::min(kChunkPtrs, count - base);
                if (!mapReadBytes(pointers + static_cast<std::uintptr_t>(base) * sizeof(std::uintptr_t),
                                  chunk, static_cast<std::size_t>(n) * sizeof(std::uintptr_t)))
                    break;
                for (int i = 0; i < n; ++i) {
                    if (chunk[i] == native) { idx = base + i; break; }
                }
            }
        }
    } else {
        std::int32_t cand = -1;
        if (mapReadI32(native + rr::UnityTransform::index, cand) && cand >= 0 && cand < 65536)
            idx = cand;
    }

    if (idx < 0) {
        static bool loggedIdxFail = false;
        if (!loggedIdxFail) {
            loggedIdxFail = true;
            std::printf("[MapProbe] %s idx resolve failed: rc=0x%llX native=0x%llX hier=0x%llX ptrs=0x%llX\n",
                tag ? tag : "?", (unsigned long long)managedRect,
                (unsigned long long)native, (unsigned long long)hierarchy, (unsigned long long)pointers);
        }
        cache.valid = false;
        return false;
    }

    // 2. Walk ancestor chain from idx up to the Canvas root one parent at a time
    // (typically 4-10 hops; avoids reading the entire Canvas parent array).
    std::vector<int> chain;
    chain.reserve(16);
    for (int cur = idx, steps = 0; cur >= 0 && cur < 131072 && steps < 32; ++steps) {
        bool cycle = false;
        for (int existing : chain) {
            if (existing == cur) { cycle = true; break; }
        }
        if (cycle) break;
        chain.push_back(cur);
        std::int32_t p = -1;
        if (!mapReadI32(parents + static_cast<std::uintptr_t>(cur) * sizeof(std::int32_t), p))
            break;
        if (p < 0 || p == cur) break;
        cur = p;
    }
    if (chain.empty()) { cache.valid = false; return false; }
    std::reverse(chain.begin(), chain.end());

    struct UiTRS {
        float px, py, pz, padP;
        float qx, qy, qz, qw;
        float sx, sy, sz, padS;
    };
    static_assert(sizeof(UiTRS) == 48);

    std::vector<UiTRS> chainTrs(chain.size());
    std::vector<std::uintptr_t> chainNative(chain.size(), 0);
    for (std::size_t i = 0; i < chain.size(); ++i) {
        if (!mapReadBytes(matrices + static_cast<std::uintptr_t>(chain[i]) * sizeof(UiTRS),
                          &chainTrs[i], sizeof(UiTRS))) {
            cache.valid = false;
            return false;
        }
        if (pointers > 0x10000) {
            chainNative[i] = mapReadPtr(
                pointers + static_cast<std::uintptr_t>(chain[i]) * sizeof(std::uintptr_t));
        }
    }
    chainNative.back() = native;

    auto rotateV = [](float qx, float qy, float qz, float qw, float vx, float vy, float vz,
                      float& ox, float& oy, float& oz) {
        const float tx = 2.f * (qy * vz - qz * vy);
        const float ty = 2.f * (qz * vx - qx * vz);
        const float tz = 2.f * (qx * vy - qy * vx);
        ox = vx + qw * tx + (qy * tz - qz * ty);
        oy = vy + qw * ty + (qz * tx - qx * tz);
        oz = vz + qw * tz + (qx * ty - qy * tx);
    };
    auto mulQ = [](float ax, float ay, float az, float aw,
                   float bx, float by, float bz, float bw,
                   float& ox, float& oy, float& oz, float& ow) {
        ox = aw * bx + ax * bw + ay * bz - az * by;
        oy = aw * by - ax * bz + ay * bw + az * bx;
        oz = aw * bz + ax * by - ay * bx + az * bw;
        ow = aw * bw - ax * bx - ay * by - az * bz;
    };

    std::vector<float> nodeWx(chainTrs.size(), 0.f);
    std::vector<float> nodeWy(chainTrs.size(), 0.f);
    std::vector<float> nodeWsx(chainTrs.size(), 1.f);
    std::vector<float> nodeWsy(chainTrs.size(), 1.f);

    float wx = 0.f, wy = 0.f, wz = 0.f;
    float rqx = 0.f, rqy = 0.f, rqz = 0.f, rqw = 1.f;
    float wsx = 1.f, wsy = 1.f, wsz = 1.f;
    for (std::size_t i = 0; i < chainTrs.size(); ++i) {
        const auto& t = chainTrs[i];
        const float n = t.qx*t.qx + t.qy*t.qy + t.qz*t.qz + t.qw*t.qw;
        if (!std::isfinite(t.px) || !std::isfinite(t.py) || !std::isfinite(t.sx) || !std::isfinite(t.sy) ||
            !std::isfinite(n) || n < 0.5f || n > 1.5f) {
            cache.valid = false;
            return false;
        }
        const float pz = std::isfinite(t.pz) ? t.pz : 0.f;
        const float sz = (std::isfinite(t.sz) && std::fabs(t.sz) > 1e-4f) ? t.sz : 1.f;
        const float inv = 1.f / std::sqrt(n);
        const float qx = t.qx * inv, qy = t.qy * inv, qz = t.qz * inv, qw = t.qw * inv;
        if (i == 0) {
            wx = t.px; wy = t.py; wz = pz;
            rqx = qx; rqy = qy; rqz = qz; rqw = qw;
            wsx = t.sx; wsy = t.sy; wsz = sz;
        } else {
            float dx = 0.f, dy = 0.f, dz = 0.f;
            rotateV(rqx, rqy, rqz, rqw, wsx * t.px, wsy * t.py, wsz * pz, dx, dy, dz);
            wx += dx; wy += dy; wz += dz;
            float nqx, nqy, nqz, nqw;
            mulQ(rqx, rqy, rqz, rqw, qx, qy, qz, qw, nqx, nqy, nqz, nqw);
            rqx = nqx; rqy = nqy; rqz = nqz; rqw = nqw;
            wsx *= t.sx; wsy *= t.sy; wsz *= sz;
        }
        nodeWx[i] = wx;
        nodeWy[i] = wy;
        nodeWsx[i] = wsx;
        nodeWsy[i] = wsy;
    }

    cache.uiCenterX = wx;
    cache.uiCenterY = wy;
    cache.uiScaleX = (std::fabs(wsx) > 1e-4f) ? std::fabs(wsx) : 1.f;
    cache.uiScaleY = (std::fabs(wsy) > 1e-4f) ? std::fabs(wsy) : 1.f;

    // 3. Read native UI::RectTransform fields (+0x30..+0x170) across the chain.
    // Unity's C++ UI::RectTransform stores five contiguous Vector2f properties:
    //   m_AnchorMin (+0x00), m_AnchorMax (+0x08), m_AnchoredPosition (+0x10),
    //   m_SizeDelta (+0x18), m_Pivot (+0x20)
    // and computes get_rect() recursively from parentSize:
    //   size = sizeDelta + parentSize * (anchorMax - anchorMin)
    //   rect = (-size * pivot, size)
    constexpr std::uintptr_t kScanBase = 0x30;
    constexpr std::size_t kScanFloats = 80; // 0x140 bytes (+0x30..+0x170)
    std::vector<std::array<float, kScanFloats>> nodeRaw(chain.size());
    std::vector<bool> hasRaw(chain.size(), false);
    for (std::size_t i = 0; i < chain.size(); ++i) {
        if (chainNative[i] > 0x10000 &&
            mapReadBytes(chainNative[i] + kScanBase, nodeRaw[i].data(), sizeof(float) * kScanFloats)) {
            hasRaw[i] = true;
        }
    }
    if (!hasRaw.back()) {
        cache.valid = false;
        return false;
    }

    auto validAnchorPivot = [](float ax0, float ay0, float ax1, float ay1,
                               float apx, float apy, float sdx, float sdy,
                               float px, float py) -> bool {
        if (!std::isfinite(ax0) || !std::isfinite(ay0) || !std::isfinite(ax1) || !std::isfinite(ay1) ||
            !std::isfinite(apx) || !std::isfinite(apy) || !std::isfinite(sdx) || !std::isfinite(sdy) ||
            !std::isfinite(px)  || !std::isfinite(py))
            return false;
        if (ax0 < -0.02f || ax0 > 1.02f || ay0 < -0.02f || ay0 > 1.02f ||
            ax1 < -0.02f || ax1 > 1.02f || ay1 < -0.02f || ay1 > 1.02f ||
            px  < -0.02f || px  > 1.02f || py  < -0.02f || py  > 1.02f)
            return false;
        if (ax1 < ax0 - 0.01f || ay1 < ay0 - 0.01f)
            return false;
        if (std::fabs(apx) > 20000.f || std::fabs(apy) > 20000.f ||
            std::fabs(sdx) > 20000.f || std::fabs(sdy) > 20000.f)
            return false;
        return true;
    };

    auto evalRectFieldsAt = [&](int fi,
                                float& outMinX, float& outMinY, float& outMaxX, float& outMaxY,
                                float& outScreenW, float& outScreenH,
                                float& outRw, float& outRh, float& outPivX, float& outPivY) -> int {
        if (fi < 0 || fi + 9 >= static_cast<int>(kScanFloats))
            return 0;

        const auto& leafF = nodeRaw.back();
        if (!validAnchorPivot(leafF[fi + 0], leafF[fi + 1], leafF[fi + 2], leafF[fi + 3],
                              leafF[fi + 4], leafF[fi + 5], leafF[fi + 6], leafF[fi + 7],
                              leafF[fi + 8], leafF[fi + 9]))
            return 0;

        int score = 20;
        std::size_t canvasIdx = 0;
        float canvasRefW = 1920.f;
        float canvasRefH = 1080.f;
        bool haveCanvasRef = false;

        // Locate the root Canvas RectTransform in chain (anchorMin=(0,0),
        // anchorMax=(0,0), pivot=(0.5,0.5), sizeDelta=(canvasRefW, canvasRefH)).
        for (std::size_t k = 0; k + 1 < chain.size(); ++k) {
            if (!hasRaw[k]) continue;
            const auto& rf = nodeRaw[k];
            const float ax0 = rf[fi + 0], ay0 = rf[fi + 1];
            const float ax1 = rf[fi + 2], ay1 = rf[fi + 3];
            const float sdx = rf[fi + 6], sdy = rf[fi + 7];
            const float px  = rf[fi + 8], py  = rf[fi + 9];
            if (std::fabs(ax0) < 0.01f && std::fabs(ay0) < 0.01f &&
                std::fabs(ax1) < 0.01f && std::fabs(ay1) < 0.01f &&
                std::fabs(px - 0.5f) < 0.01f && std::fabs(py - 0.5f) < 0.01f &&
                sdx >= 600.f && sdx <= 8000.f && sdy >= 360.f && sdy <= 5000.f) {
                const float aspect = sdx / sdy;
                if (aspect >= 1.1f && aspect <= 3.8f) {
                    canvasIdx = k;
                    canvasRefW = sdx;
                    canvasRefH = sdy;
                    haveCanvasRef = true;
                    score += 200;
                    break;
                }
            }
        }

        if (!haveCanvasRef) {
            for (std::size_t k = 0; k + 1 < chain.size(); ++k) {
                if (nodeWx[k] >= 300.f && nodeWx[k] <= 4000.f &&
                    nodeWy[k] >= 180.f && nodeWy[k] <= 2500.f &&
                    std::fabs(nodeWsx[k]) > 1e-4f && std::fabs(nodeWsy[k]) > 1e-4f) {
                    canvasIdx = k;
                    canvasRefW = (2.f * nodeWx[k]) / std::fabs(nodeWsx[k]);
                    canvasRefH = (2.f * nodeWy[k]) / std::fabs(nodeWsy[k]);
                    haveCanvasRef = true;
                    break;
                }
            }
        }

        float pw = canvasRefW;
        float ph = canvasRefH;
        float ppivX = 0.5f;
        float ppivY = 0.5f;
        float cx = 0.f;
        float cy = 0.f;
        float csx = 1.f;
        float csy = 1.f;

        for (std::size_t k = canvasIdx + 1; k < chain.size(); ++k) {
            const float tx = chainTrs[k].px;
            const float ty = chainTrs[k].py;
            cx += csx * tx;
            cy += csy * ty;
            if (std::fabs(chainTrs[k].sx) > 1e-4f) csx *= chainTrs[k].sx;
            if (std::fabs(chainTrs[k].sy) > 1e-4f) csy *= chainTrs[k].sy;

            if (!hasRaw[k]) continue;
            const auto& rf = nodeRaw[k];
            const float ax0 = rf[fi + 0], ay0 = rf[fi + 1];
            const float ax1 = rf[fi + 2], ay1 = rf[fi + 3];
            const float apx = rf[fi + 4], apy = rf[fi + 5];
            const float sdx = rf[fi + 6], sdy = rf[fi + 7];
            const float px  = rf[fi + 8], py  = rf[fi + 9];
            if (!validAnchorPivot(ax0, ay0, ax1, ay1, apx, apy, sdx, sdy, px, py)) {
                if (k + 1 == chain.size()) return 0;
                continue;
            }

            // Unity uGUI invariant: localPosition.xy == anchoredPosition + anchorRef
            const float expX = apx + pw * (ax0 + (ax1 - ax0) * px - ppivX);
            const float expY = apy + ph * (ay0 + (ay1 - ay0) * py - ppivY);
            const float err  = std::fabs(expX - tx) + std::fabs(expY - ty);
            if (err < 2.5f) {
                score += 160;
            } else if (err < 12.f) {
                score += 60;
            } else if (haveCanvasRef && err > 90.f) {
                return 0;
            }

            pw = sdx + pw * (ax1 - ax0);
            ph = sdy + ph * (ay1 - ay0);
            ppivX = px;
            ppivY = py;
        }

        const float rw = pw;
        const float rh = ph;
        const float absCsx = (std::fabs(csx) > 1e-4f) ? std::fabs(csx) : 1.f;
        const float absCsy = (std::fabs(csy) > 1e-4f) ? std::fabs(csy) : 1.f;
        const float sw = rw * absCsx;
        const float sh = rh * absCsy;
        if (!std::isfinite(sw) || !std::isfinite(sh) ||
            sw < 50.f || sw > 8000.f || sh < 50.f || sh > 8000.f)
            return 0;

        // Standard UI pivots (0, 0.5, 1) and square/screen-sized containers earn bonus confidence.
        auto isStandard01 = [](float v) {
            return std::fabs(v) < 0.01f || std::fabs(v - 0.5f) < 0.01f || std::fabs(v - 1.f) < 0.01f;
        };
        if (isStandard01(ppivX) && isStandard01(ppivY))
            score += 40;
        if (std::fabs(sw - sh) < 6.f)
            score += 30;

        const float rx = -rw * ppivX;
        const float ry = -rh * ppivY;
        outMinX = (canvasRefW * 0.5f) + cx + rx * absCsx;
        outMaxX = outMinX + sw;
        outMinY = (canvasRefH * 0.5f) + cy + ry * absCsy;
        outMaxY = outMinY + sh;
        outScreenW = canvasRefW;
        outScreenH = canvasRefH;
        outRw = rw;
        outRh = rh;
        outPivX = ppivX;
        outPivY = ppivY;
        return score;
    };

    float minX = 0.f, minY = 0.f, maxX = 0.f, maxY = 0.f;
    float screenW = 0.f, screenH = 0.f;
    float rw = 0.f, rh = 0.f, pivX = 0.5f, pivY = 0.5f;
    bool foundRect = false;

    if (s_rectTransformRectOffset >= static_cast<std::int32_t>(kScanBase)) {
        const int fi = (s_rectTransformRectOffset - static_cast<std::int32_t>(kScanBase)) / 4;
        if (evalRectFieldsAt(fi, minX, minY, maxX, maxY, screenW, screenH, rw, rh, pivX, pivY) >= 100)
            foundRect = true;
    }
    if (!foundRect) {
        int bestScore = 0;
        int bestFi = -1;
        for (int fi = 0; fi + 9 < static_cast<int>(kScanFloats); ++fi) {
            float tMinX, tMinY, tMaxX, tMaxY, tSW, tSH, tRw, tRh, tPx, tPy;
            const int score = evalRectFieldsAt(fi, tMinX, tMinY, tMaxX, tMaxY, tSW, tSH, tRw, tRh, tPx, tPy);
            if (score > bestScore) {
                bestScore = score;
                bestFi = fi;
                minX = tMinX; minY = tMinY; maxX = tMaxX; maxY = tMaxY;
                screenW = tSW; screenH = tSH;
                rw = tRw; rh = tRh; pivX = tPx; pivY = tPy;
            }
        }
        if (bestFi >= 0 && bestScore >= 60) {
            s_rectTransformRectOffset = static_cast<std::int32_t>(kScanBase) + bestFi * 4;
            foundRect = true;
            std::printf("[MapProbe] RectTransform fields discovered at native+0x%X (score=%d): size=(%.1fx%.1f) pivot=(%.2f,%.2f) canvas=(%.0fx%.0f) bounds=(%.0f,%.0f..%.0f,%.0f)\n",
                s_rectTransformRectOffset, bestScore, rw, rh, pivX, pivY, screenW, screenH, minX, minY, maxX, maxY);
        }
    }

    if (!foundRect) {
        static bool loggedRectFail = false;
        if (!loggedRectFail || forceRefresh) {
            loggedRectFail = true;
            std::printf("[MapProbe] %s RectTransform probe missed: native=0x%llX idx=%d chain=%zu wx=(%.1f,%.1f) scale=(%.3f,%.3f)\n",
                tag ? tag : "?", (unsigned long long)native, idx, chain.size(), wx, wy, cache.uiScaleX, cache.uiScaleY);
        }
        cache.valid = false;
        return false;
    }

    cache.uiMinX = minX;
    cache.uiMaxX = maxX;
    cache.uiMinY = minY;
    cache.uiMaxY = maxY;
    cache.uiScreenW = screenW;
    cache.uiScreenH = screenH;
    cache.valid = (cache.uiMaxX - cache.uiMinX >= 50.f) && (cache.uiMaxY - cache.uiMinY >= 50.f);
    return cache.valid;
}

static void ReadInGameMapState()
{
    const auto& rcfg = ConfigInstance.Radar;
    if (!rcfg.InGameMinimap && !rcfg.InGameFullMap) {
        inGameMapState = InGameMapState{};
        return;
    }
    if (cachedBattleModel <= 0x10000) {
        inGameMapState = InGameMapState{};
        return;
    }

    if (cachedMapDialogModel <= 0x10000) {
        std::uintptr_t dlg = 0;
        if (!TargetProcess.ReadFresh(cachedBattleModel + rr::BattleModel::MapDialogModel, &dlg, sizeof(dlg)))
            dlg = ReadPtr(cachedBattleModel + rr::BattleModel::MapDialogModel);
        if (dlg > 0x10000 && IsClass(dlg, "MapDialogModel"))
            cachedMapDialogModel = dlg;
    }
    if (cachedMapConfigModel <= 0x10000) {
        std::uintptr_t cfg = 0;
        if (!TargetProcess.ReadFresh(cachedBattleModel + rr::BattleModel::MapConfigModel, &cfg, sizeof(cfg)))
            cfg = ReadPtr(cachedBattleModel + rr::BattleModel::MapConfigModel);
        if (cfg > 0x10000 && IsClass(cfg, "MapConfigModel")) {
            cachedMapConfigModel = cfg;
            if (!TargetProcess.ReadFresh(cfg + rr::MapConfigModel::FullScreen, &cachedMapFullVar, sizeof(cachedMapFullVar)))
                cachedMapFullVar = ReadPtr(cfg + rr::MapConfigModel::FullScreen);
            if (!TargetProcess.ReadFresh(cfg + rr::MapConfigModel::Minimap, &cachedMapMiniVar, sizeof(cachedMapMiniVar)))
                cachedMapMiniVar = ReadPtr(cfg + rr::MapConfigModel::Minimap);
            if (!TargetProcess.ReadFresh(cfg + rr::MapConfigModel::MapModel, &cachedMapModelPtr, sizeof(cachedMapModelPtr)))
                cachedMapModelPtr = ReadPtr(cfg + rr::MapConfigModel::MapModel);
        }
    }
    if (cachedMapConfigModel <= 0x10000) {
        inGameMapState = InGameMapState{};
        return;
    }

    // Read ScreenState fresh so pressing/closing 'M' responds on the very next frame.
    bool fullOpen = false;
    if (cachedMapDialogModel > 0x10000) {
        std::int32_t state = 0;
        if (TargetProcess.ReadFresh(cachedMapDialogModel + rr::ScreenModel::ScreenState, &state, sizeof(state)))
            fullOpen = (state == 1);
    }

    const bool modeFlipped = (inGameMapState.fullscreenOpen != fullOpen);
    inGameMapState.fullscreenOpen = fullOpen;

    if (cachedMapModelPtr > 0x10000) {
        float nz = 0.f;
        if (TargetProcess.Read(cachedMapModelPtr + rr::MapModel::NormalizedZoom, &nz, sizeof(nz)) && std::isfinite(nz))
            inGameMapState.normalizedZoom = nz;
    }

    auto readVariance = [&](std::uintptr_t varPtr, InGameMapVariance& out, RectTransformCache& rCache, const char* tag) {
        out = InGameMapVariance{};
        if (varPtr <= 0x10000) return;

        struct VarBlock {
            float posX, posY;
            float rotation;
            float cameraSize;
            std::uintptr_t resizeContainer;
        };
        static_assert(sizeof(VarBlock) == 24);
        VarBlock blk{};
        if (!TargetProcess.ReadFresh(varPtr + rr::MapConfigVariance::Position, &blk, sizeof(blk)))
            return;
        if (!std::isfinite(blk.posX) || !std::isfinite(blk.posY) ||
            !std::isfinite(blk.rotation) || !std::isfinite(blk.cameraSize) ||
            blk.cameraSize <= 0.1f || blk.cameraSize > 50000.f)
            return;

        out.position = Vector2(blk.posX, blk.posY);
        out.rotation = blk.rotation;
        out.cameraSize = blk.cameraSize;
        out.valid = true;

        if (blk.resizeContainer > 0x10000 &&
            ReadRectTransformUiBounds(blk.resizeContainer, rCache, modeFlipped, tag))
        {
            out.rectValid = true;
            out.uiMinX = rCache.uiMinX;
            out.uiMinY = rCache.uiMinY;
            out.uiMaxX = rCache.uiMaxX;
            out.uiMaxY = rCache.uiMaxY;
            out.uiCenterX = rCache.uiCenterX;
            out.uiCenterY = rCache.uiCenterY;
            out.uiScaleX = rCache.uiScaleX;
            out.uiScaleY = rCache.uiScaleY;
            out.uiScreenW = rCache.uiScreenW;
            out.uiScreenH = rCache.uiScreenH;
        }

        // One-shot / mode-flip diagnostic log for 2nd-PC verification.
        static bool loggedMini = false;
        static bool loggedFull = false;
        bool& loggedFlag = (tag[0] == 'F') ? loggedFull : loggedMini;
        if (!loggedFlag || modeFlipped) {
            loggedFlag = true;
            const float camYawDeg = cameraState.valid
                ? (std::atan2(cameraState.forward.x, cameraState.forward.z) * 57.2957795f)
                : 0.f;
            std::printf("[MapProbe] %s: pos=(%.1f,%.1f) local=(%.1f,%.1f) rot=%.2f camYaw=%.1fdeg camSize=%.1f rc=0x%llX rect=%d(%.0f,%.0f..%.0f,%.0f / %.0fx%.0f)\n",
                tag, out.position.x, out.position.y,
                localPosition.x, localPosition.z,
                out.rotation, camYawDeg, out.cameraSize,
                (unsigned long long)blk.resizeContainer,
                out.rectValid ? 1 : 0,
                out.uiMinX, out.uiMinY, out.uiMaxX, out.uiMaxY,
                out.uiScreenW, out.uiScreenH);
        }
    };

    if (fullOpen) {
        if (rcfg.InGameFullMap)
            readVariance(cachedMapFullVar, inGameMapState.fullscreen, g_fullRectCache, "Full");
    } else {
        if (rcfg.InGameMinimap)
            readVariance(cachedMapMiniVar, inGameMapState.minimap, g_miniRectCache, "Mini");
    }

    inGameMapState.valid = fullOpen ? inGameMapState.fullscreen.valid : inGameMapState.minimap.valid;
    const bool fastFull = fullOpen && rcfg.InGameFullMap && rcfg.Hide3DEspOnFullMap;
    g_fullMapFastMode.store(fastFull, std::memory_order_relaxed);
    if (fullOpen && rcfg.InGameFullMap && cachedMapFullVar > 0x10000) {
        g_liveFullMapVarPtr.store(cachedMapFullVar, std::memory_order_release);
        if (inGameMapState.fullscreen.valid) {
            std::lock_guard<std::mutex> lk(g_liveFullMapMutex);
            g_liveFullMap.pos = inGameMapState.fullscreen.position;
            g_liveFullMap.rot = inGameMapState.fullscreen.rotation;
            g_liveFullMap.camSize = inGameMapState.fullscreen.cameraSize;
            g_liveFullMap.valid = true;
        }
    } else {
        g_liveFullMapVarPtr.store(0, std::memory_order_release);
        std::lock_guard<std::mutex> lk(g_liveFullMapMutex);
        g_liveFullMap.valid = false;
    }
}

static void ReadFastVehicles()
{
    const auto& rcfg = ConfigInstance.Radar;
    const bool wantMapVehicles = (rcfg.InGameMinimap || rcfg.InGameFullMap) && rcfg.InGameMapVehicles;
    const bool wantWorldVehicles = ConfigInstance.World.Vehicles || ConfigInstance.LootFilter.Enabled;
    if ((!wantMapVehicles && !wantWorldVehicles) || cachedBattleEntities <= 0x10000)
    {
        fastVehicles.clear();
        return;
    }

    const std::uint64_t now = GetTickCount64();
    if (g_fastVehMetaStamp == 0 || now - g_fastVehMetaStamp >= 500) {
        g_fastVehMetaStamp = now;
        g_fastVehMeta.clear();

        const std::uintptr_t worldPtr = ReadPtr(cachedBattleEntities + 0x60);
        if (worldPtr <= 0x10000) {
            fastVehicles.clear();
            return;
        }

        auto readVehDict = [&](std::uintptr_t offset, std::vector<RobRunMemory::DictEntry>& out) {
            out.clear();
            const std::uintptr_t field = ReadPtr(worldPtr + offset);
            if (field <= 0x10000) return;
            if (RobRunMemory::ReadDictInt(field, out, false)) return;
            const std::uintptr_t inner = ReadPtr(field + 0x10);
            if (inner > 0x10000 && inner != field && RobRunMemory::ReadDictInt(inner, out, false)) return;
            const std::uintptr_t inner18 = ReadPtr(field + 0x18);
            if (inner18 > 0x10000 && inner18 != field && inner18 != inner)
                RobRunMemory::ReadDictInt(inner18, out, false);
        };

        auto resolvePosAddr = [](std::uintptr_t transform) -> std::uintptr_t {
            if (transform <= 0x10000) return 0;
            const std::uintptr_t wrapper = ReadPtr(transform + rr::VehicleTransformModel::Position);
            if (wrapper > 0x10000 && wrapper < 0x7FFFFFFFFFFFull) {
                Vector3 test{};
                if (Il2Cpp::ReadPrimitiveVector3(wrapper, test) && RobRunMemory::Finite(test)) {
                    // Most primitive<Vector3> wrappers store Value at +0x18 or +0x10.
                    Vector3 v18{}, v10{};
                    if (RobRunMemory::ReadVector3(wrapper + 0x18, v18) &&
                        std::fabs(v18.x - test.x) < 0.01f && std::fabs(v18.z - test.z) < 0.01f)
                        return wrapper + 0x18;
                    if (RobRunMemory::ReadVector3(wrapper + 0x10, v10) &&
                        std::fabs(v10.x - test.x) < 0.01f && std::fabs(v10.z - test.z) < 0.01f)
                        return wrapper + 0x10;
                }
            }
            Vector3 inlineV{};
            if (RobRunMemory::ReadVector3(transform + rr::VehicleTransformModel::Position, inlineV) &&
                RobRunMemory::Finite(inlineV))
                return transform + rr::VehicleTransformModel::Position;
            return 0;
        };

        auto readOwnerPublicId = [](std::uintptr_t ownerModel) -> std::uint64_t {
            if (ownerModel <= 0x10000) return 0;
            const std::uintptr_t pubWrapper = ReadPtr(ownerModel + rr::VehicleOwnerModel::OwnerPublicId);
            if (pubWrapper <= 0x10000) return 0;
            std::int64_t pubId = 0;
            if (!Il2Cpp::ReadPrimitiveInt64(pubWrapper, pubId) || pubId <= 0)
                return 0;
            const std::int64_t emptyId = RobRunMemory::ReadInt64(ownerModel + rr::VehicleOwnerModel::EmptyPublicId);
            if (pubId == emptyId)
                return 0;
            return static_cast<std::uint64_t>(pubId);
        };

        std::unordered_set<int> seenIds;
        auto collect = [&](std::uintptr_t dictOff, RobRunWorld::Kind kind,
                           std::uintptr_t transformOff, std::uintptr_t ownerOff,
                           std::uintptr_t ownerRoleOff, std::uintptr_t ctrlOwnerOff) {
            std::vector<RobRunMemory::DictEntry> entries;
            readVehDict(dictOff, entries);
            for (const auto& e : entries) {
                if (e.value <= 0x10000 || !seenIds.insert(e.key).second) continue;
                const std::uintptr_t transform = ReadPtr(e.value + transformOff);
                const std::uintptr_t posAddr = resolvePosAddr(transform);
                if (posAddr <= 0x10000) continue;

                FastVehicleEntry meta{};
                meta.id = e.key;
                meta.kind = kind;
                meta.model = e.value;
                meta.posAddr = posAddr;
                if (ownerOff != 0)
                    meta.ownerPublicId = readOwnerPublicId(ReadPtr(e.value + ownerOff));
                if (ownerRoleOff != 0) {
                    EntityData roleScratch{};
                    ApplyRoleFromWrapper(ReadPtr(e.value + ownerRoleOff), roleScratch);
                    meta.ownerRoleName = std::move(roleScratch.roleName);
                }
                if (ctrlOwnerOff != 0) {
                    const std::uintptr_t ctrl = ReadPtr(e.value + ctrlOwnerOff);
                    if (ctrl > 0x10000) {
                        std::uintptr_t drv = 0;
                        if (!TargetProcess.ReadFresh(ctrl + 0x18, &drv, sizeof(drv)))
                            drv = ReadPtr(ctrl + 0x18);
                        meta.hasDriver = (drv > 0x10000);
                    }
                }
                g_fastVehMeta.push_back(std::move(meta));
            }
        };

        // Direct private dictionary fields on WorldEntitiesModel:
        // _cars (+0x60), _helicopters (+0x68), _vehicles (+0x58)
        collect(rr::WorldEntitiesModel::Cars, RobRunWorld::Kind::Car,
                rr::CarModel::Transform, rr::CarModel::CharacterOwner,
                rr::CarModel::OwnerRole, rr::CarModel::ControlOwner);
        collect(rr::WorldEntitiesModel::Helicopters, RobRunWorld::Kind::Helicopter,
                rr::HelicopterModel::Transform, 0, 0, 0);
        collect(rr::WorldEntitiesModel::Vehicles, RobRunWorld::Kind::Vehicle,
                rr::VehicleModel::Transform, rr::VehicleModel::CharacterOwner,
                rr::VehicleModel::OwnerRole, 0);
    }

    fastVehicles.clear();
    if (g_fastVehMeta.empty())
        return;

    std::vector<Vector3> positions(g_fastVehMeta.size());
    {
        Memory::ScopedScatter sc(TargetProcess, /*cached=*/false);
        if (sc.handle) {
            for (std::size_t i = 0; i < g_fastVehMeta.size(); ++i) {
                TargetProcess.AddScatterReadRequest(sc,
                    g_fastVehMeta[i].posAddr, &positions[i], sizeof(Vector3));
            }
            TargetProcess.ExecuteScatterRead(sc);
        } else {
            for (std::size_t i = 0; i < g_fastVehMeta.size(); ++i) {
                if (!TargetProcess.ReadFresh(g_fastVehMeta[i].posAddr, &positions[i], sizeof(Vector3)))
                    RobRunMemory::ReadVector3(g_fastVehMeta[i].posAddr, positions[i]);
            }
        }
    }

    fastVehicles.reserve(g_fastVehMeta.size());
    for (std::size_t i = 0; i < g_fastVehMeta.size(); ++i) {
        const auto& m = g_fastVehMeta[i];
        const Vector3& pos = positions[i];
        if (!RobRunMemory::Finite(pos) || (pos.x == 0.f && pos.y == 0.f && pos.z == 0.f) ||
            std::fabs(pos.x) > 1.0e6f || std::fabs(pos.z) > 1.0e6f)
            continue;
        FastVehicle fv{};
        fv.id = m.id;
        fv.kind = m.kind;
        fv.position = pos;
        fv.ownerPublicId = m.ownerPublicId;
        fv.ownerRoleName = m.ownerRoleName;
        fv.hasDriver = m.hasDriver;
        if (m.ownerPublicId > 0) {
            const auto nickIt = g_publicIdToNick.find(m.ownerPublicId);
            if (nickIt != g_publicIdToNick.end())
                fv.ownerNicknameUtf8 = nickIt->second;
        }
        fastVehicles.push_back(std::move(fv));
    }
}

static void UpdateInternal()
{
    const std::uint64_t tPassStart = SteadyMicros();
    std::uint64_t tRootsUs = 0, tViewsUs = 0, tP1Us = 0, tP2Us = 0, tP3Us = 0, tDetUs = 0, tP5Us = 0, tMatchUs = 0;
    int passRebuilds = 0;
    int passFallbacks = 0;

    static int uiCount = 0;
    ++uiCount;

    entities.clear();
    localEntityId = -1;
    localPosition = Vector3{};
    fallbackViewProjection = Matrix4x4{};
    fallbackViewProjectionValid = false;
    poseFailureCount = 0;
    boneTotalAcc = boneInvalidAcc = boneNormZeroAcc = boneNormBadAcc = 0;
    bonePosInfAcc = boneChainDropAcc = 0;
    poseScatterOkAcc = poseScatterFailAcc = poseFreshOkAcc = 0;

    if (!gaBase) { if (uiCount <= 3) DLOG("UpdateInternal #%d: no gaBase", uiCount); return; }

    if ((uiCount % 200) == 1)
        DLOG("UpdateInternal #%d: game=0x%llx bm=0x%llx be=0x%llx entities=%zu gen=%llu",
             uiCount, (unsigned long long)cachedGame,
             (unsigned long long)cachedBattleModel,
             (unsigned long long)cachedBattleEntities,
             entities.size(), (unsigned long long)sessionGeneration);

    ++updateCounter;
    if ((updateCounter % 120) == 0)
        RevalidateSession();

    static int gameResolveFails = 0;
    static std::uint64_t lastWaitLogMs = 0;
    if (!cachedGame) {
        if (gameRetryCountdown <= 0) {
            cachedGame = ResolveGameViaGom();
            // The GOM walk is the most expensive resolution step. When it
            // fails (menu/loading screen) back off instead of walking 8k nodes
            // on every pass.
            gameRetryCountdown = cachedGame ? 0 : 30;
            if (!cachedGame) {
                if (gameResolveFails == 0)
                    DLOG("GameRoot: GOM resolve failed, backing off");
                const std::uint64_t nowMs = GetTickCount64();
                if (lastWaitLogMs == 0 || nowMs - lastWaitLogMs >= 5000) {
                    lastWaitLogMs = nowMs;
                    std::printf("[Reader] Waiting for Game root (GOM walk failed, attempt %d)...\n",
                        gameResolveFails + 1);
                }
                if (++gameResolveFails >= 20) {
                    DLOG("GameRoot: sustained failure (%d attempts), flushing DMA caches & retrying CR3 fix", gameResolveFails);
                    std::printf("[Reader] Sustained GOM failure (%d attempts) — flushing DMA caches & retrying CR3 fix...\n",
                        gameResolveFails);
                    gameResolveFails = 0;
                    if (!CheckGameProcessAlive(true))
                        return;
                    MaybeRefreshDmaCaches("gomResolveFail", 2000);
                    TargetProcess.FixCr3();
                }
            } else {
                gameResolveFails = 0;
                lastWaitLogMs = 0;
            }
        } else {
            --gameRetryCountdown;
        }
        if (!cachedGame)
            CheckGameProcessAlive();
    } else {
        gameResolveFails = 0;
    }

    if (uiCount <= 3) DLOG("UpdateInternal #%d: game=0x%llx", uiCount, (unsigned long long)cachedGame);

    if (!cachedBattleModel)
        cachedBattleModel = ResolveBattleModel(cachedGame);

    // Unity swaps BattleModel and BattleEntitiesModel on every map load. The old
    // objects are freed but still pass a class check, so a stale pointer looks
    // healthy and the ESP keeps reading the previous map. Follow the Game pointer
    // with uncached reads every 2s so location rejoins recover immediately even
    // under -norefresh.
    static std::uint64_t lastMapCheck = 0;
    const std::uint64_t nowTick = GetTickCount64();
    const bool mapCheckDue = (nowTick - lastMapCheck) >= 2000;
    if (mapCheckDue) lastMapCheck = nowTick;
    if (cachedGame && mapCheckDue) {
        const std::uintptr_t gameModel = ReadPtrFresh(cachedGame + rr::Game::GameModel);
        const std::uintptr_t freshBattle = gameModel > 0x10000
            ? ReadPtrFresh(gameModel + rr::GameModel::BattleModel) : 0;
        // Hysteresis: only adopt a new model once the same pointer shows up on two
        // consecutive updates. Without this a transient read or a fallback-resolved
        // pointer would be treated as a map change and clear the world every frame.
        static std::uintptr_t pendingBattle = 0;
        static int pendingBattleHits = 0;
        static int emptyBattleHits = 0;
        if (freshBattle > 0x10000 && freshBattle != cachedBattleModel) {
            emptyBattleHits = 0;
            if (freshBattle == pendingBattle) ++pendingBattleHits;
            else { pendingBattle = freshBattle; pendingBattleHits = 1; }
        } else if (freshBattle <= 0x10000 && cachedBattleModel > 0x10000) {
            pendingBattle = 0;
            pendingBattleHits = 0;
            if (++emptyBattleHits >= 2) {
                DLOG("ReaderRoot: BattleModel cleared on scene exit (gameModel=0x%llx)",
                     (unsigned long long)gameModel);
                cachedBattleModel = 0;
                cachedBattleEntities = 0;
                cachedUpdater = 0;
                cachedLocalCharacter = 0;
                cachedCameraModel = 0;
                cachedLookDirectionModel = 0;
                cachedLookDirDirect = 0;
                cachedPlayerRotationInput = 0;
                emptyBattleHits = 0;
                if (gameModel <= 0x10000 && !IsClass(cachedGame, "Game"))
                    cachedGame = 0;
                MarkSessionChanged();
            }
        } else {
            pendingBattle = 0;
            pendingBattleHits = 0;
            emptyBattleHits = 0;
        }
        if (freshBattle > 0x10000 && freshBattle != cachedBattleModel && pendingBattleHits >= 2 &&
            IsClass(freshBattle, "BattleModel")) {
            DLOG("ReaderRoot: BattleModel changed 0x%llx -> 0x%llx (state change)",
                 (unsigned long long)cachedBattleModel, (unsigned long long)freshBattle);
            cachedBattleModel = freshBattle;
            cachedBattleEntities = 0;
            cachedUpdater = 0;
            cachedLocalCharacter = 0;
            cachedCameraModel = 0;
            cachedLookDirectionModel = 0;
            cachedLookDirDirect = 0;
            cachedPlayerRotationInput = 0;
            worldRefreshStamp = 0;
            inventoryRefreshStamp = 0;
            MarkSessionChanged();
            std::lock_guard<std::mutex> lock(g_worldMutex);
            g_worldState = std::make_shared<RobRunWorld::State>();
            g_plotState = std::make_shared<RobRunWorld::PlotState>();
        } else if (cachedBattleModel > 0x10000 && cachedBattleEntities > 0x10000) {
            // Same BattleModel, but the entities model under it may have been
            // rebuilt (entering or leaving a raid without a sector change).
            const std::uintptr_t freshEntities = ReadPtrFresh(
                cachedBattleModel + rr::BattleModel::BattleEntitiesModel);
            if (freshEntities > 0x10000 && freshEntities != cachedBattleEntities &&
                IsClass(freshEntities, "BattleEntitiesModel")) {
                DLOG("ReaderRoot: BattleEntitiesModel changed 0x%llx -> 0x%llx",
                     (unsigned long long)cachedBattleEntities, (unsigned long long)freshEntities);
                cachedBattleEntities = freshEntities;
                cachedLocalCharacter = 0;
                worldRefreshStamp = 0;
            }
        }
    }

    if (!cachedBattleModel) {
        if (!CheckGameProcessAlive())
            return;
        const std::uint64_t nowMs = GetTickCount64();
        if (cachedGame > 0x10000 && (lastWaitLogMs == 0 || nowMs - lastWaitLogMs >= 5000)) {
            lastWaitLogMs = nowMs;
            std::printf("[Reader] Game=0x%llX — waiting for BattleModel (in menu/loading)...\n",
                (unsigned long long)cachedGame);
        }
        {
            std::lock_guard<std::mutex> lock(g_worldMutex);
            g_worldState = std::make_shared<RobRunWorld::State>();
            g_plotState = std::make_shared<RobRunWorld::PlotState>();
            g_inventoryState = std::make_shared<RobRunInventory::State>();
        }
        worldRefreshStamp = 0;
        inventoryRefreshStamp = 0;
        // The old camera/input pointers are meaningless once BattleModel is
        // gone. Drop them so a stale pointer can never be advertised to aim.
        cachedCameraModel = 0;
        cachedLookDirectionModel = 0;
        cachedLookDirDirect = 0;
        cachedPlayerRotationInput = 0;
        cachedLocalCharacter = 0;
        MaybeRefreshDmaCaches("noBattleModel", 2000);
        NoteSessionState();
        ++failureCount;
        DLOG("ReaderRoot: BattleModel lost, failures=%u game=0x%llx",
             failureCount, (unsigned long long)cachedGame);
        return;
    }

    if (!cachedBattleEntities) {
        if (battleEntitiesRetryCountdown <= 0) {
            const std::uintptr_t candidate = ReadPtrFresh(
                cachedBattleModel + rr::BattleModel::BattleEntitiesModel);
            if (IsClass(candidate, "BattleEntitiesModel"))
                cachedBattleEntities = candidate;
            else
                battleEntitiesRetryCountdown = 15;
        } else {
            --battleEntitiesRetryCountdown;
        }
    }
    if (cachedBattleEntities <= 0x10000) {
        if (!CheckGameProcessAlive())
            return;
        cachedCameraModel = 0;
        cachedLookDirectionModel = 0;
        cachedLookDirDirect = 0;
        cachedPlayerRotationInput = 0;
        cachedLocalCharacter = 0;
        MaybeRefreshDmaCaches("noBattleEntities", 2000);
        NoteSessionState();
        ++failureCount;
        return;
    }

    {
        const std::uint64_t locNow = GetTickCount64();
        if (locationStamp == 0 || locNow - locationStamp >= 5000) {
            locationStamp = locNow;
            const std::uintptr_t settings = ReadPtr(
                cachedBattleModel + rr::BattleModel::BattleSettings);
            if (settings > 0x10000) {
                const std::uintptr_t idStr = ReadPtr(
                    settings + rr::BattleSettingsDescription::Id);
                std::wstring wide;
                if (RobRunMemory::ReadString(idStr, wide)) {
                    std::string id = RobRunMemory::WideToUtf8(wide);
                    bool mapChanged = false;
                    {
                        std::lock_guard<std::mutex> lk(locationMutex);
                        if (id != cachedLocationId) {
                            DLOG("Location: '%s' -> '%s'", cachedLocationId.c_str(), id.c_str());
                            mapChanged = !cachedLocationId.empty() && !id.empty();
                        }
                        cachedLocationId = std::move(id);
                    }
                    if (mapChanged) {
                        MarkSessionChanged();
                        MaybeRefreshDmaCaches("locationChanged", 0);
                        cachedViews.clear();
                        cachedViewOwner.clear();
                        viewOwnerMisses.clear();
                        rigCache.clear();
                        viewRefreshStamp = 0;
                        slotRefreshStamp = 0;
                        fullResolveStamp = 0;
                    }
                }
            }
        }
    }

    {
        const std::uint64_t camNow = GetTickCount64();
        if (!cachedCameraModel || camNow - cameraResolveStamp >= 500) {
            cameraResolveStamp = camNow;
            const std::uintptr_t cameraCandidate = ReadPtr(
                cachedBattleModel + rr::BattleModel::BattleCameraModel);
            cachedCameraModel =
                (cameraCandidate > 0x10000 &&
                 IsClass(cameraCandidate, "BattleCameraModel"))
                ? cameraCandidate : 0;
        }
    }
    g_renderCameraModel.store(cachedCameraModel, std::memory_order_release);
    {
        // When the dedicated camera thread is streaming a fresh native camera,
        // avoid issuing two extra uncached DMA reads (BattleCameraModel +
        // CinemachineBrain FOV) on every entity worker pass.
        const NativeCameraSnapshot nativeSnap = LoadNativeCamera();
        const std::uint64_t nowUs = SteadyMicros();
        const bool nativeFresh = ConfigInstance.Player.NativeEspCamera &&
            nativeSnap.valid && nativeSnap.poseValid &&
            nowUs >= nativeSnap.stampUs &&
            (nowUs - nativeSnap.stampUs) < 500000ULL;
        if (nativeFresh && cameraState.valid) {
            cameraState.position = nativeSnap.position;
            cameraState.forward = nativeSnap.forward;
        } else {
            cameraState = RobRunCamera::Read(cachedCameraModel);
        }
    }

    // Resolve the native Unity camera fallback here, on the worker, so the
    // render thread never walks the GameObjectManager when the managed camera
    // path is unavailable.
    if (!cameraState.valid) {
        const auto fallback = CameraFinder::GetCameraMatrices();
        if (fallback.valid) {
            fallbackViewProjection = fallback.viewMatrix;
            fallbackViewProjectionValid = true;
        }
    }

    const std::uintptr_t localPlayer = ReadPtr(
        cachedBattleEntities + rr::BattleEntitiesModel::LocalPlayerEntityModel);
    if (localPlayer > 0x10000) {
        localEntityId = RobRunMemory::ReadInt32(localPlayer + rr::PlayerEntityModel::EntityId);
        cachedLocalCharacter = ReadPtr(
            localPlayer + rr::PlayerEntityModel::BattleCharacterModel);
        if (cachedLocalCharacter > 0x10000) {
            const std::uintptr_t transform = ReadPtr(
                cachedLocalCharacter + rr::BattleCharacterModel::CharacterTransformModel);
            struct LocalTransformBlock {
                Vector3 position;
                float pitch;
                float yaw;
            } localBlock{};
            const std::uintptr_t positionAddress =
                transform + rr::CharacterTransformModel::Position;
            if ((RobRunMemory::ReadBytesFresh(positionAddress, &localBlock, sizeof(localBlock)) ||
                 RobRunMemory::ReadBytes(positionAddress, &localBlock, sizeof(localBlock))) &&
                RobRunMemory::Finite(localBlock.position)) {
                localPosition = localBlock.position;
                if (std::isfinite(localBlock.yaw)) localYaw = localBlock.yaw;
                if (std::isfinite(localBlock.pitch)) localPitch = localBlock.pitch;
            }

            {
                const std::uint64_t chainNow = GetTickCount64();
                if (!cachedLookDirectionModel || chainNow - localChainStamp >= 500) {
                    localChainStamp = chainNow;
                    const std::uintptr_t dataModel = ReadPtr(
                        cachedLocalCharacter + rr::BattleCharacterModel::Data);
                    if (dataModel > 0x10000)
                        cachedLookDirectionModel = ReadPtr(
                            dataModel + rr::CommonModel::LookDirection);
                    else
                        cachedLookDirectionModel = 0;

                    cachedLookDirDirect = ReadPtr(cachedLocalCharacter + rr::BattleCharacterModel::LookDirection);
                    if (cachedLookDirDirect <= 0x10000) cachedLookDirDirect = 0;

                    if (cachedBattleModel > 0x10000)
                        cachedPlayerRotationInput = ReadPtr(cachedBattleModel + rr::BattleModel::PlayerRotationInput);
                    else
                        cachedPlayerRotationInput = 0;
                    if (cachedPlayerRotationInput <= 0x10000) cachedPlayerRotationInput = 0;
                }
            }
        }
    } else {
        cachedLocalCharacter = 0;
        cachedLookDirectionModel = 0;
        cachedLookDirDirect = 0;
        cachedPlayerRotationInput = 0;
    }

    // Detect full respawn / fast-travel / zone teleport: a >25 m instant jump
    // in local position means Unity streamed in new character views and bone
    // hierarchies on freshly mapped pages whose TLB/Mem cache entries may be
    // stale under -norefresh.
    {
        static Vector3 s_prevLocalPos{};
        static bool s_hadLocalPos = false;
        const bool nowValid = RobRunMemory::Finite(localPosition) && !localPosition.IsZero();
        if (nowValid) {
            const bool jumped = s_hadLocalPos && Distance(s_prevLocalPos, localPosition) > 25.f;
            if (jumped) {
                DLOG("Respawn/Teleport detected (jump=%.1fm), refreshing DMA caches & views",
                     Distance(s_prevLocalPos, localPosition));
                MaybeRefreshDmaCaches("respawnTeleport", 1500);
                cachedViews.clear();
                cachedViewOwner.clear();
                viewOwnerMisses.clear();
                rigCache.clear();
                viewRefreshStamp = 0;
                slotRefreshStamp = 0;
                fullResolveStamp = 0;
            }
            s_prevLocalPos = localPosition;
            s_hadLocalPos = true;
        } else {
            s_hadLocalPos = false;
        }
    }

    const std::uint64_t tAfterRoots = SteadyMicros();
    tRootsUs = tAfterRoots - tPassStart;

    // Live in-game minimap / fullscreen ('M') map camera & rect state (~2 reads)
    // plus high-frequency vehicle scatter (~0.3 ms) for smooth map vehicles.
    ReadInGameMapState();
    ReadFastVehicles();

    // Drop a stale updater when the scene changes; otherwise the view
    // dictionary silently stays empty across a map transition.
    if (cachedUpdater > 0x10000 &&
        ReadPtr(cachedUpdater + rr::BattleEntitiesUpdater::_battleModel) != cachedBattleModel) {
        cachedUpdater = 0;
    }
    if (!cachedUpdater) {
        if (updaterRetryCountdown <= 0) {
            cachedUpdater = FindUpdater(cachedGame, cachedBattleModel);
            updaterRetryCountdown = cachedUpdater ? 0 : 15;
        } else {
            --updaterRetryCountdown;
        }
    }

    {
        // The view is registered per entity id by the updater; it is not
        // cached by the model. Re-read it often enough that a respawn or a
        // pooled character cannot keep the old rig for long.
        const std::uint64_t viewNow = GetTickCount64();
        if (cachedUpdater > 0x10000 &&
            (viewRefreshStamp == 0 || viewNow - viewRefreshStamp >= 400))
        {
            viewRefreshStamp = viewNow;
            const std::uintptr_t viewDictionary = ReadPtr(
                cachedUpdater + rr::BattleEntitiesUpdater::_characterEntityViews);
            std::vector<RobRunMemory::DictEntry> viewEntries;
            if (RobRunMemory::ReadDictInt(viewDictionary, viewEntries)) {
                std::unordered_map<std::int32_t, std::uintptr_t> fresh;
                fresh.reserve(viewEntries.size());
                for (const auto& entry : viewEntries)
                    fresh[entry.key] = entry.value;
                cachedViews.swap(fresh);
            }
            // Match after this pass's player positions exist (end of pass);
            // matching here would only see the previous list, which is cleared
            // at the top of every pass.
            viewsRefreshed = true;
        }
    }
    const auto& views = cachedViewOwner;

    {
        const std::uint64_t slotNow = GetTickCount64();
        if (slotRefreshStamp == 0 || slotNow - slotRefreshStamp >= 3000)
        {
            slotRefreshStamp = slotNow;

            std::vector<RobRunMemory::DictEntry> slotEntries;
            bool slotsRead = false;

            // 1. ClientCharacterSlotsModel on BattleModel (+0x248) -> _slotByEntityId (+0x18)
            const std::uintptr_t slotsModel = ReadPtr(
                cachedBattleModel + rr::BattleModel::CharacterSlotsModel);
            if (slotsModel > 0x10000) {
                std::uintptr_t slotDictionary = ReadPtr(
                    slotsModel + rr::CharacterSlotsModel::SlotDict);
                slotsRead = RobRunMemory::ReadDictInt(slotDictionary, slotEntries);
                if (!slotsRead || slotEntries.empty()) {
                    slotDictionary = ReadPtr(
                        slotsModel + rr::CharacterSlotsModel::SlotDictLegacy);
                    slotsRead = RobRunMemory::ReadDictInt(slotDictionary, slotEntries);
                }
            }

            // 2. ClientCharacterSlotsModel via TypeInfo static singleton
            if (!slotsRead || slotEntries.empty()) {
                const std::uintptr_t typeInfo = ReadPtr(gaBase + rr::TypeInfo::ClientCharacterSlotsModel);
                if (typeInfo > 0x10000) {
                    const std::uintptr_t statics = ReadPtr(typeInfo + rr::Klass::static_fields);
                    if (statics > 0x10000) {
                        const std::uintptr_t clientSlots = ReadPtr(statics);
                        if (clientSlots > 0x10000) {
                            const std::uintptr_t slotDict = ReadPtr(clientSlots + rr::CharacterSlotsModel::SlotDict);
                            slotsRead = RobRunMemory::ReadDictInt(slotDict, slotEntries);
                        }
                    }
                }
            }

            // 3. BattleCharacterSlotsModel on BattleModel (+0x4D0)
            if (!slotsRead || slotEntries.empty()) {
                const std::uintptr_t battleSlotsModel = ReadPtr(
                    cachedBattleModel + rr::BattleModel::CharacterSlotsModelLegacy);
                if (battleSlotsModel > 0x10000) {
                    std::uintptr_t slotDict = ReadPtr(battleSlotsModel + 0x18);
                    if (slotDict <= 0x10000) slotDict = ReadPtr(battleSlotsModel + 0x10);
                    std::vector<RobRunMemory::RefDictEntry> refEntries;
                    if (RobRunMemory::ReadDictRef(slotDict, refEntries) && !refEntries.empty()) {
                        for (const auto& re : refEntries) {
                            if (re.value <= 0x10000) continue;
                            const std::uintptr_t slotData = ReadPtr(re.value + rr::BattleCharacterSlotModel::_data);
                            if (slotData > 0x10000) {
                                const std::uintptr_t entityIdWrap = ReadPtr(slotData + rr::BattleCharacterSlotData::EntityId);
                                std::int32_t entId = -1;
                                if (entityIdWrap > 0x10000 && Il2Cpp::ReadPrimitiveInt(entityIdWrap, entId) && entId >= 0) {
                                    slotEntries.push_back({ 0, 0, entId, 0, re.value });
                                    slotsRead = true;
                                }
                            }
                        }
                    }
                }
            }

            // 4. BattleEntitiesModel CharacterSlotsRoot fallback
            if (!slotsRead || slotEntries.empty()) {
                const std::uintptr_t slotsRoot = ReadPtr(
                    cachedBattleEntities + rr::BattleEntitiesModel::CharacterSlotsRoot);
                if (slotsRoot > 0x10000) {
                    const std::uintptr_t slotDictionary = ReadPtr(
                        slotsRoot + rr::CharacterSlotsRoot::SlotDict);
                    slotsRead = RobRunMemory::ReadDictInt(slotDictionary, slotEntries);
                }
            }

            if (slotsRead && !slotEntries.empty()) {
                std::unordered_map<std::int32_t, std::uintptr_t> fresh;
                fresh.reserve(slotEntries.size());
                for (const auto& entry : slotEntries) {
                    if (entry.key >= 0 && entry.value > 0x10000)
                        fresh[entry.key] = entry.value;
                }
                if (!fresh.empty()) {
                    cachedSlots.swap(fresh);
                    // Prime publicId -> nickname map for all server slots so far
                    // moving vehicles can display their owner's name on the map.
                    for (const auto& kv : cachedSlots) {
                        EntityData slotScratch{};
                        ReadSlotIdentity(kv.second, slotScratch);
                    }
                }
            }
        }
    }
    const auto& slots = cachedSlots;

    // Party membership is refreshed at 1 Hz. The local public id comes from the
    // player entity model; the slot dictionary is the fallback because slot
    // data always carries a raw int64 public id.
    std::uint64_t localPublicId = 0;
    if (localPlayer > 0x10000) {
        const std::int64_t value = RobRunMemory::ReadInt64(
            localPlayer + rr::PlayerEntityModel::PublicId);
        if (value > 0) localPublicId = static_cast<std::uint64_t>(value);
    }
    if (localPublicId == 0 && localEntityId > 0) {
        const auto localSlot = slots.find(localEntityId);
        if (localSlot != slots.end()) {
            const std::int64_t value = RobRunMemory::ReadInt64(
                localSlot->second + rr::BattleCharacterSlotData::PublicId);
            if (value > 0) localPublicId = static_cast<std::uint64_t>(value);
        }
    }
    const std::uint64_t partyNow = GetTickCount64();
    if (g_partyStamp == 0 || partyNow - g_partyStamp >= 1000) {
        g_partyStamp = partyNow;
        RefreshParty(cachedGame, localEntityId, localPublicId);
    }

    const std::uint64_t tAfterViews = SteadyMicros();
    tViewsUs = tAfterViews - tAfterRoots;

    if (uiCount <= 3) DLOG("UpdateInternal #%d: pre-entity, battleModel=0x%llx, battleEntities=0x%llx",
        uiCount, (unsigned long long)cachedBattleModel, (unsigned long long)cachedBattleEntities);

    const std::uintptr_t entityDictionary = ReadPtr(
        cachedBattleEntities + rr::BattleEntitiesModel::EntityDict);
    std::vector<RobRunMemory::DictEntry> entityEntries;
    static int entityDictFailures = 0;
    if (RobRunMemory::ReadDictInt(entityDictionary, entityEntries)) {
        entityDictFailures = 0;

        // Also merge any BattleCharacterModels from WorldEntitiesModel
        // (_battleCharacters at +0x18 and _notSpawnedBattleCharacters at +0x10)
        // that aren't in BattleEntitiesModel._models so the fullscreen map sees
        // every character known to the client regardless of 3D view spawn state.
        const std::uintptr_t worldModelPtr = ReadPtr(cachedBattleEntities + 0x60);
        if (worldModelPtr > 0x10000) {
            std::unordered_set<std::int32_t> seenIds;
            seenIds.reserve(entityEntries.size() + 32);
            for (const auto& e : entityEntries) seenIds.insert(e.key);
            for (std::uintptr_t dictOff : { std::uintptr_t(0x18), std::uintptr_t(0x10) }) {
                const std::uintptr_t charDict = ReadPtr(worldModelPtr + dictOff);
                if (charDict <= 0x10000) continue;
                std::vector<RobRunMemory::DictEntry> extra;
                if (!RobRunMemory::ReadDictInt(charDict, extra, false)) continue;
                for (const auto& e : extra) {
                    if (e.value > 0x10000 && seenIds.insert(e.key).second)
                        entityEntries.push_back(e);
                }
            }
        }

        entities.reserve(entityEntries.size());

        struct TransformBlock {
            Vector3 position;
            float pitch;
            float yaw;
        };
        static_assert(sizeof(TransformBlock) == 20, "transform block layout");

        struct EntityWork {
            std::int32_t entityId = 0;
            std::uintptr_t characterModel = 0;
            std::uintptr_t transform = 0;
            std::uintptr_t blockAddress = 0;
            TransformBlock block{};
            bool positionValid = false;
            bool needsPose = false;
            std::size_t poseOffset = 0;
            std::size_t poseSize = 0;
        };

        // ---------------------------------------------------------------
        // Phase 1a: Scatter-read characterModel pointers for all
        // entities. One device transaction replaces N sequential reads.
        // Also pre-read SimulatedModel so the fallback path avoids an
        // extra round trip for new/changed entities.
        // ---------------------------------------------------------------
        struct PreResolve {
            std::int32_t key = 0;
            std::uintptr_t entityModel = 0;
            std::uintptr_t clientModel = 0;
            std::uintptr_t serverModel = 0;
            std::uintptr_t simulatedModel = 0;
            std::uintptr_t slotsModel = 0;
        };
        std::vector<PreResolve> pre;
        pre.reserve(entityEntries.size());
        std::vector<std::size_t> needResolve;

        const std::uint64_t resolveNow = GetTickCount64();
        const bool forceFullResolve = resolveNow - fullResolveStamp >= 2000;
        if (forceFullResolve) fullResolveStamp = resolveNow;

        for (const auto& entry : entityEntries) {
            if (entry.value <= 0x10000) continue;
            PreResolve p{ entry.key, entry.value, 0, 0, 0, 0 };
            if (!forceFullResolve) {
                const auto cd = detailCache.find(entry.key);
                if (cd != detailCache.end() && cd->second.characterModel > 0x10000)
                    p.clientModel = cd->second.characterModel;
                else
                    needResolve.push_back(pre.size());
            } else {
                needResolve.push_back(pre.size());
            }
            pre.push_back(p);
        }

        if (!needResolve.empty()) {
            Memory::ScopedScatter sc(TargetProcess, /*cached=*/true);
            if (sc.handle) {
                for (auto idx : needResolve) {
                    TargetProcess.AddScatterReadRequest(sc,
                        pre[idx].entityModel + rr::BattleCharacterEntityModel::Slots,
                        &pre[idx].slotsModel, sizeof(std::uintptr_t));
                    TargetProcess.AddScatterReadRequest(sc,
                        pre[idx].entityModel + rr::BattleCharacterEntityModel::ClientModel,
                        &pre[idx].clientModel, sizeof(std::uintptr_t));
                    TargetProcess.AddScatterReadRequest(sc,
                        pre[idx].entityModel + rr::BattleCharacterEntityModel::ServerModel,
                        &pre[idx].serverModel, sizeof(std::uintptr_t));
                    TargetProcess.AddScatterReadRequest(sc,
                        pre[idx].entityModel + rr::BattleCharacterEntityModel::SimulatedModel,
                        &pre[idx].simulatedModel, sizeof(std::uintptr_t));
                }
                TargetProcess.ExecuteScatterReadCached(sc);
            }
        }

        for (auto idx : needResolve) {
            auto& p = pre[idx];
            std::uintptr_t characterModel = 0;

            // Priority 1: ServerModel (+0x28) is the true BattleCharacterModel with RobberInventoryCost, PrivateModel, Data
            if (p.serverModel > 0x10000 && IsClass(p.serverModel, "BattleCharacterModel"))
                characterModel = p.serverModel;
            // Priority 2: SimulatedModel (+0x30)
            else if (p.simulatedModel > 0x10000 && IsClass(p.simulatedModel, "BattleCharacterModel"))
                characterModel = p.simulatedModel;
            // Priority 3: ClientModel (+0x20)
            else if (p.clientModel > 0x10000 && (IsClass(p.clientModel, "BattleCharacterModel") || IsClass(p.clientModel, "BattleCharacterClientModel")))
                characterModel = p.clientModel;
            // Priority 4: Direct entityModel
            else if (p.entityModel > 0x10000 && IsClass(p.entityModel, "BattleCharacterModel"))
                characterModel = p.entityModel;

            p.clientModel = characterModel;

            // Populate cachedSlots directly from this entity's _slots if cachedSlots is still empty
            if (p.slotsModel > 0x10000 && cachedSlots.empty()) {
                const std::uintptr_t slotDict = ReadPtr(p.slotsModel + rr::CharacterSlotsModel::SlotDict);
                std::vector<RobRunMemory::DictEntry> directEntries;
                if (slotDict > 0x10000 && RobRunMemory::ReadDictInt(slotDict, directEntries)) {
                    for (const auto& se : directEntries) {
                        if (se.key >= 0 && se.value > 0x10000)
                            cachedSlots[se.key] = se.value;
                    }
                }
            }
        }

        // ---------------------------------------------------------------
        // Phase 1b: Scatter-read transform pointers. Entities whose
        // transform was cached from the previous pass skip the read.
        // ---------------------------------------------------------------
        std::vector<EntityWork> work;
        work.reserve(pre.size());
        std::vector<std::size_t> needTransform;

        for (const auto& p : pre) {
            if (p.clientModel <= 0x10000) continue;
            EntityWork w{};
            w.entityId = p.key;
            w.characterModel = p.clientModel;
            const auto cd = detailCache.find(p.key);
            if (cd != detailCache.end() &&
                cd->second.characterModel == p.clientModel &&
                cd->second.transform > 0x10000)
            {
                w.transform = cd->second.transform;
                w.blockAddress = w.transform +
                    rr::CharacterTransformModel::Position;
            } else {
                needTransform.push_back(work.size());
            }
            work.push_back(w);
        }

        if (!needTransform.empty()) {
            Memory::ScopedScatter sc(TargetProcess, /*cached=*/true);
            if (sc.handle) {
                for (auto idx : needTransform)
                    TargetProcess.AddScatterReadRequest(sc,
                        work[idx].characterModel +
                            rr::BattleCharacterModel::CharacterTransformModel,
                        &work[idx].transform, sizeof(std::uintptr_t));
                TargetProcess.ExecuteScatterReadCached(sc);
            }
            for (auto idx : needTransform) {
                if (work[idx].transform > 0x10000) {
                    work[idx].blockAddress = work[idx].transform +
                        rr::CharacterTransformModel::Position;
                    detailCache[work[idx].entityId].transform =
                        work[idx].transform;
                }
            }
        }

        {
            std::size_t dst = 0;
            for (std::size_t src = 0; src < work.size(); ++src) {
                if (work[src].transform > 0x10000) {
                    if (dst != src) work[dst] = std::move(work[src]);
                    ++dst;
                }
            }
            work.resize(dst);
        }

        // ---------------------------------------------------------------
        // Pre-pass: collect pose requests from cached rig state so
        // positions and poses can be read in one device transaction.
        // ---------------------------------------------------------------
        std::vector<unsigned char> poseBuf;
        std::size_t poseTotal = 0;

        const bool haveLocalPos = !localPosition.IsZero();
        const float poseCullDist = static_cast<float>(ConfigInstance.Player.MaxDistance) + 25.0f;
        const bool fullMapFastMode = g_fullMapFastMode.load(std::memory_order_relaxed);

        if (!fullMapFastMode)
        for (auto& w : work) {
            // Skip expensive multi-KB skeleton reads for the local player,
            // already-dead entities, and players far past ESP MaxDistance.
            if (w.entityId == localEntityId) continue;
            const auto dIt = detailCache.find(w.entityId);
            if (dIt != detailCache.end() && dIt->second.isDead) continue;

            const auto viewIt = views.find(w.entityId);
            const std::uintptr_t viewPtr =
                (viewIt != views.end()) ? viewIt->second : 0;
            auto rigIt = rigCache.find(w.entityId);
            if (rigIt != rigCache.end() && rigIt->second.layoutValid &&
                rigIt->second.view == viewPtr)
            {
                if (haveLocalPos && !rigIt->second.lastSampledPos.IsZero() &&
                    Distance(rigIt->second.lastSampledPos, localPosition) > poseCullDist)
                    continue;
                const std::size_t sz = Skeleton::PoseDataSize(rigIt->second.layout);
                w.needsPose = true;
                w.poseOffset = poseTotal;
                w.poseSize = sz;
                poseTotal += sz;
            }
        }
        poseBuf.resize(poseTotal, 0);

        const std::uint64_t tAfterP1 = SteadyMicros();
        tP1Us = tAfterP1 - tAfterViews;

        // ---------------------------------------------------------------
        // Phase 2: Chunked scatter-read of positions AND poses.
        // Splitting into small batches (max 4 pose-bearing entities per
        // transaction) prevents a full lobby from locking the FPGA bus for
        // 15-25 ms at once, letting the high-priority camera thread interleave
        // sub-millisecond physical camera reads between chunks.
        // ---------------------------------------------------------------
        if (!work.empty()) {
            Memory::ScopedScatter scatter(TargetProcess);
            if (scatter.handle) {
                constexpr int kMaxPosesPerChunk = 4;
                constexpr int kMaxEntsPerChunk = 12;
                int chunkPoses = 0;
                int chunkEnts = 0;
                for (std::size_t wi = 0; wi < work.size(); ++wi) {
                    auto& w = work[wi];
                    TargetProcess.AddScatterReadRequest(scatter,
                        w.blockAddress, &w.block, sizeof(TransformBlock));
                    if (w.needsPose) {
                        auto& rig = rigCache[w.entityId];
                        TargetProcess.AddScatterReadRequest(scatter,
                            rig.layout.matrices,
                            poseBuf.data() + w.poseOffset, w.poseSize);
                        ++chunkPoses;
                    }
                    ++chunkEnts;
                    const bool flush = (wi + 1 == work.size()) ||
                        chunkPoses >= kMaxPosesPerChunk ||
                        chunkEnts >= kMaxEntsPerChunk;
                    if (flush) {
                        TargetProcess.ExecuteReadScatter(scatter);
                        chunkPoses = 0;
                        chunkEnts = 0;
                        if (wi + 1 < work.size())
                            SwitchToThread();
                    }
                }
            }
        }

        for (auto& w : work) {
            if (!RobRunMemory::Finite(w.block.position) ||
                (w.block.position.x == 0.f && w.block.position.y == 0.f &&
                 w.block.position.z == 0.f))
            {
                w.block = TransformBlock{};
                if (!RobRunMemory::ReadBytesFresh(w.blockAddress, &w.block,
                        sizeof(TransformBlock)))
                {
                    w.positionValid = false;
                    continue;
                }
            }
            w.positionValid = RobRunMemory::Finite(w.block.position);
        }

        if (uiCount <= 3) DLOG("UpdateInternal #%d: phase2 done, %zu work items", uiCount, work.size());

        const std::uint64_t tAfterP2 = SteadyMicros();
        tP2Us = tAfterP2 - tAfterP1;

        // ---------------------------------------------------------------
        // Phase 3: Rig layout rebuilds and late pose collection.
        // Layouts already valid had their pose read above; newly
        // rebuilt layouts get a supplementary scatter.
        // Budgeted to at most 1 layout build/upgrade per pass so a wave
        // of spawns or refreshes can never stall the worker thread.
        // ---------------------------------------------------------------
        const std::uint64_t now = GetTickCount64();
        std::vector<std::size_t> lateResolve;
        int rebuildBudget = fullMapFastMode ? 0 : 1;

        if (!fullMapFastMode)
        for (std::size_t wi = 0; wi < work.size(); ++wi) {
            auto& w = work[wi];
            if (!w.positionValid) continue;
            if (w.entityId == localEntityId) continue;
            const auto dIt = detailCache.find(w.entityId);
            if (dIt != detailCache.end() && dIt->second.isDead) continue;
            if (haveLocalPos && Distance(w.block.position, localPosition) > poseCullDist)
                continue;

            const auto viewIt = views.find(w.entityId);
            const std::uintptr_t viewPtr =
                (viewIt != views.end()) ? viewIt->second : 0;

            auto& rig = rigCache[w.entityId];
            if (rig.view != viewPtr) {
                rig.view = viewPtr;
                rig.layoutValid = false;
                rig.layoutStamp = 0;
                rig.upgradeAttempts = 0;
                rig.bones.clear();
                rig.points = Skeleton::BonePoints{};
                rig.prevLocalValid = false;
                rig.smoothedValid = false;
                rig.smoothedWorldMatrices.clear();
                rig.lastSampledPos = Vector3{};
                rig.lastMotionTick = 0;
                rig.animatorActive = true;
                rig.poseHistCount = 0;
                rig.poseHistCursor = 0;
            }

            // Skeleton layout rebuild is wrapped in try/catch because the
            // DMA reads through the entity view hierarchy can trigger AVs
            // on some hardware setups. With /EHa this catch handles SEH.
            try {
            auto limbScore = [](const Skeleton::RigLayout& l) -> int {
                int s = 0;
                if (l.leftHip >= 0)       s += 4;
                if (l.leftKnee >= 0)      s += 4;
                if (l.leftFoot >= 0)      s += 2;
                if (l.rightHip >= 0)      s += 4;
                if (l.rightKnee >= 0)     s += 4;
                if (l.rightFoot >= 0)     s += 2;
                if (l.leftShoulder >= 0)  s += 2;
                if (l.leftElbow >= 0)     s += 2;
                if (l.leftHand >= 0)      s += 1;
                if (l.rightShoulder >= 0) s += 2;
                if (l.rightElbow >= 0)    s += 2;
                if (l.rightHand >= 0)     s += 1;
                return s;
            };
            auto layoutMissingLimbs = [](const Skeleton::RigLayout& l) -> bool {
                return l.leftHip < 0 || l.rightHip < 0 ||
                       l.leftKnee < 0 || l.rightKnee < 0 ||
                       l.leftShoulder < 0 || l.rightShoulder < 0;
            };

            auto buildBestLayout = [&](const Skeleton::ReadFn& readFn,
                                       Skeleton::RigLayout& best) -> bool {
                bool ok = Skeleton::BuildLayoutFromView(viewPtr, readFn, best);
                if (!ok) {
                    ok = Skeleton::BuildLayoutByName(viewPtr, readFn, best);
                } else if (layoutMissingLimbs(best)) {
                    Skeleton::RigLayout byName;
                    if (Skeleton::BuildLayoutByName(viewPtr, readFn, byName) &&
                        limbScore(byName) > limbScore(best)) {
                        best = std::move(byName);
                    }
                }
                return ok;
            };

            const bool incomplete = rig.layoutValid && layoutMissingLimbs(rig.layout) &&
                rig.upgradeAttempts < 4;
            const std::uint64_t upgradeIntervalMs =
                (rig.upgradeAttempts == 0) ? 500 :
                (rig.upgradeAttempts == 1) ? 1500 : 4000;
            const std::uint64_t checkIntervalMs = incomplete ? upgradeIntervalMs : 1500;
            if (rig.layoutValid && now - rig.layoutStamp >= checkIntervalMs) {
                if (viewPtr <= 0x10000 ||
                    !Skeleton::ValidateHierarchy(rig.layout, DmaRead)) {
                    rig.layoutStamp = now;
                    rig.layoutValid = false;
                    rig.layoutStamp = 0;
                    rig.upgradeAttempts = 0;
                } else if (incomplete && rebuildBudget > 0) {
                    rig.layoutStamp = now;
                    --rebuildBudget;
                    ++passRebuilds;
                    ++rig.upgradeAttempts;
                    // Silently try upgrading an incomplete layout using cached
                    // DMA (bone name lookup inside uses 3 fast scatter calls).
                    Skeleton::RigLayout candidate;
                    const bool ok = buildBestLayout(DmaRead, candidate);
                    SwitchToThread();
                    if (ok && limbScore(candidate) > limbScore(rig.layout)) {
                        const bool sameBuf =
                            candidate.matrices == rig.layout.matrices &&
                            candidate.count == rig.layout.count;
                        rig.layout = std::move(candidate);
                        rig.poseHistCount = 0;
                        rig.poseHistCursor = 0;
                        rig.smoothedValid = false;
                        if (!layoutMissingLimbs(rig.layout))
                            rig.upgradeAttempts = 0;
                        if (!sameBuf) w.needsPose = false;
                    }
                } else if (!incomplete) {
                    rig.layoutStamp = now;
                }
            }
            const bool dueForRebuild = !rig.layoutValid && viewPtr > 0x10000 &&
                rebuildBudget > 0 &&
                (rig.layoutStamp == 0 || now - rig.layoutStamp >= 250);
            if (dueForRebuild) {
                rig.layoutStamp = now;
                --rebuildBudget;
                ++passRebuilds;
                Skeleton::RigLayout candidate;
                const bool ok = buildBestLayout(DmaRead, candidate);
                SwitchToThread();
                if (ok) {
                    rig.layout = std::move(candidate);
                    rig.layoutValid = true;
                    rig.poseFailures = 0;
                    rig.poseFailureSince = 0;
                    rig.smoothedValid = false;
                    rig.poseHistCount = 0;
                    rig.poseHistCursor = 0;
                    rig.upgradeAttempts = 0;
                } else {
                    if (rig.poseFailureSince == 0)
                        rig.poseFailureSince = now;
                    if (now - rig.poseFailureSince >= 2500) {
                        rig.bones.clear();
                        rig.points = Skeleton::BonePoints{};
                        rig.smoothedValid = false;
                        rig.smoothedWorldMatrices.clear();
                        rig.poseHistCount = 0;
                        rig.poseHistCursor = 0;
                    }
                }
            }
            } catch (...) {}

            if (rig.layoutValid && !w.needsPose) {
                const std::size_t sz = Skeleton::PoseDataSize(rig.layout);
                w.needsPose = true;
                w.poseOffset = poseTotal;
                w.poseSize = sz;
                poseTotal += sz;
                lateResolve.push_back(wi);
            }
        }

        if (!lateResolve.empty()) {
            poseBuf.resize(poseTotal, 0);
            Memory::ScopedScatter scatter(TargetProcess);
            if (scatter.handle) {
                for (auto idx : lateResolve) {
                    auto& rig = rigCache[work[idx].entityId];
                    TargetProcess.AddScatterReadRequest(scatter,
                        rig.layout.matrices,
                        poseBuf.data() + work[idx].poseOffset,
                        work[idx].poseSize);
                }
                TargetProcess.ExecuteReadScatter(scatter);
            }
        }

        const std::uint64_t tAfterP3 = SteadyMicros();
        tP3Us = tAfterP3 - tAfterP2;

        // ---------------------------------------------------------------
        // Phase 5: Process poses, build entities, wall check.
        // Cap periodic detail refreshes per pass so a full lobby never
        // stalls the loop with 20 sequential refresh chains at once.
        // ---------------------------------------------------------------
        // Freshest camera for the occlusion pass. The render thread re-reads
        // the camera every frame, so its copy is newer than the worker's ~30 Hz
        // snapshot; using it keeps "visible from the camera" in step with the
        // frame the overlay draws over.
        RobRunCamera::CameraState visCamera = cameraState;
        RobRunCamera::CameraState freshVisCamera{};
        if (GetFreshCamera(freshVisCamera)) visCamera = freshVisCamera;
        if (uiCount <= 3) DLOG("UpdateInternal #%d: entering entity detail loop", uiCount);
        int detailBudget = 8;
        const std::int64_t battleNowMs = ReadBattleNowMs(cachedBattleEntities);
        const bool carCabinSolid = ConfigInstance.Player.CarCabinSolid;
        CollisionBVH::DynamicOccluders dynCopy;
        {
            std::lock_guard<std::mutex> lock(g_dynOccludersMutex);
            dynCopy = g_dynOccluders;
        }

        // --- Scatter-batched detail refresh ---
        // Collect entities needing detail refresh, then read all their
        // data in 4 scatter phases (4 DMA round-trips) instead of
        // ~50 sequential reads per entity.
        struct ScatterEnt {
            std::size_t workIdx;
            int entityId;
            std::uintptr_t characterModel;
            // phase 1: from characterModel
            std::uintptr_t privateModel{};
            std::uintptr_t data{};
            std::uintptr_t isDeadWrapper{};
            std::uintptr_t weaponReload{};
            std::uintptr_t itemInHands{};
            std::uintptr_t hasArmorWrapper{};
            std::uintptr_t inVehicle{};
            std::uintptr_t isBgReloadWrapper{};
            std::uintptr_t playerRole{};
            std::uintptr_t costModel{};
            std::uintptr_t knockout{};
            std::uintptr_t knockdown{};
            std::uintptr_t kdLeaveModel{};
            // phase 2: second level
            std::uintptr_t healthModel{};
            std::uintptr_t initArmorModel{};
            std::uintptr_t staminaModel{};
            std::uintptr_t common{};
            std::uintptr_t publicData{};
            std::uintptr_t vehicleWrapper{};
            std::uintptr_t reloadState{};
            std::uintptr_t reloadEndTsW{};
            std::uintptr_t kdEndTsW{};
            std::uintptr_t koPhase{};
            std::uintptr_t koEndTsW{};
            std::uint8_t   koSurrendered{};
            std::uint8_t   koHandcuffing{};
            std::uint8_t   koHandcuffed{};
            std::uintptr_t weaponCurrent{};
            // phase 3: third level + blocks
            struct { std::uintptr_t hw; std::uintptr_t dw; std::int32_t maxAmt; std::int32_t critHp; } hb{};
            std::uintptr_t staminaW{};
            std::uintptr_t armorW{};
            std::uintptr_t pubHasArmorW{};
            std::uintptr_t commonRole{};
            std::uintptr_t roleDesc{};
            std::uintptr_t firingW{};
            std::uintptr_t sprintingW{};
            std::uintptr_t crouchingW{};
            std::uintptr_t deadCommonW{};
            std::uintptr_t vehIdW{};
            std::uintptr_t reloadDesc{};
            std::uintptr_t koPhaseDesc{};
            std::uintptr_t weaponData{};
            struct { std::int32_t charInv, vehInv, charEquip, charInvCash, charInvItems; } costs{};
            // phase 4: wrapper values
            float healthVal{};
            std::uint8_t deadVal{};
            std::int32_t staminaVal{};
            std::int32_t armorVal{};
            std::uint8_t hasArmorVal{};
            std::uint8_t pubHasArmorVal{};
            std::uint8_t firingVal{};
            std::uint8_t sprintingVal{};
            std::uint8_t crouchingVal{};
            std::uint8_t deadCommonVal{};
            std::int32_t vehIdCommon{};
            std::int32_t vehIdRole{};
            std::uint8_t bgReloadVal{};
            std::int64_t reloadEndTs{};
            std::int64_t kdEndTs{};
            std::uint8_t kdLeaveActive{};
            std::int64_t koEndTs{};
            std::uintptr_t weaponDesc{};
        };
        ScatterEnt se[8];
        int seCount = 0;
        std::unordered_map<int, int> seIdx;
        seIdx.reserve(8);
        for (auto& w : work) {
            if (!w.positionValid) continue;
            auto& det = detailCache[w.entityId];
            const bool modelChanged = det.characterModel != w.characterModel;
            const bool expired = now - det.stamp > 500;
            if (!modelChanged && !(expired && detailBudget > 0)) continue;
            if (!modelChanged) --detailBudget;
            if (modelChanged) {
                det = DetailCacheEntry{};
                det.characterModel = w.characterModel;
                det.transform = w.transform;
            }
            det.stamp = now;
            auto& s = se[seCount];
            std::memset(&s, 0, sizeof(s));
            s.workIdx = static_cast<std::size_t>(&w - work.data());
            s.entityId = w.entityId;
            s.characterModel = w.characterModel;
            seIdx[w.entityId] = seCount;
            ++seCount;
            if (seCount >= 8) break;
        }

        if (seCount > 0) {
            Memory::ScopedScatter sc(TargetProcess, /*cached=*/true);
            if (sc.handle) {
                // Phase 1: characterModel-level pointers
                for (int i = 0; i < seCount; ++i) {
                    auto& s = se[i];
                    const auto cm = s.characterModel;
                    TargetProcess.AddScatterReadRequest(sc, cm + rr::BattleCharacterModel::PrivateModel, &s.privateModel, 8);
                    TargetProcess.AddScatterReadRequest(sc, cm + rr::BattleCharacterModel::Data, &s.data, 8);
                    TargetProcess.AddScatterReadRequest(sc, cm + rr::BattleCharacterModel::IsDeadWrapper, &s.isDeadWrapper, 8);
                    TargetProcess.AddScatterReadRequest(sc, cm + rr::BattleCharacterModel::WeaponReload, &s.weaponReload, 8);
                    TargetProcess.AddScatterReadRequest(sc, cm + rr::BattleCharacterModel::ItemInHands, &s.itemInHands, 8);
                    TargetProcess.AddScatterReadRequest(sc, cm + rr::BattleCharacterModel::HasArmor, &s.hasArmorWrapper, 8);
                    TargetProcess.AddScatterReadRequest(sc, cm + rr::BattleCharacterModel::InVehicle, &s.inVehicle, 8);
                    TargetProcess.AddScatterReadRequest(sc, cm + rr::BattleCharacterModel::IsBackgroundReload, &s.isBgReloadWrapper, 8);
                    TargetProcess.AddScatterReadRequest(sc, cm + rr::BattleCharacterModel::PlayerRole, &s.playerRole, 8);
                    TargetProcess.AddScatterReadRequest(sc, cm + rr::BattleCharacterModel::RobberInventoryCost, &s.costModel, 8);
                    TargetProcess.AddScatterReadRequest(sc, cm + rr::BattleCharacterModel::Knockout, &s.knockout, 8);
                    TargetProcess.AddScatterReadRequest(sc, cm + rr::BattleCharacterModel::Knockdown, &s.knockdown, 8);
                    TargetProcess.AddScatterReadRequest(sc, cm + rr::BattleCharacterModel::KnockdownLeaveState, &s.kdLeaveModel, 8);
                }
                if (!TargetProcess.ExecuteScatterReadCached(sc))
                    DLOG("ScatterFail: detail phase 1 (seCount=%d)", seCount);
                SwitchToThread();

                // Phase 2: second-level pointers
                for (int i = 0; i < seCount; ++i) {
                    auto& s = se[i];
                    if (s.privateModel > 0x10000) {
                        TargetProcess.AddScatterReadRequest(sc, s.privateModel + rr::BattleCharacterPrivateModel::HealthModel, &s.healthModel, 8);
                        TargetProcess.AddScatterReadRequest(sc, s.privateModel + rr::BattleCharacterPrivateModel::InitArmorModel, &s.initArmorModel, 8);
                        TargetProcess.AddScatterReadRequest(sc, s.privateModel + rr::BattleCharacterPrivateModel::Stamina, &s.staminaModel, 8);
                    }
                    if (s.data > 0x10000) {
                        TargetProcess.AddScatterReadRequest(sc, s.data + rr::BattleData::Common, &s.common, 8);
                        TargetProcess.AddScatterReadRequest(sc, s.data + 0x30, &s.publicData, 8);
                    }
                    if (s.inVehicle > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.inVehicle + rr::InVehicleModel::EntityId, &s.vehicleWrapper, 8);
                    if (s.weaponReload > 0x10000) {
                        TargetProcess.AddScatterReadRequest(sc, s.weaponReload + rr::WeaponReloadModel::State, &s.reloadState, 8);
                        TargetProcess.AddScatterReadRequest(sc, s.weaponReload + rr::WeaponReloadModel::EndTs, &s.reloadEndTsW, 8);
                    }
                    if (s.knockdown > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.knockdown + rr::KnockdownModel::EndTs, &s.kdEndTsW, 8);
                    if (s.knockout > 0x10000) {
                        TargetProcess.AddScatterReadRequest(sc, s.knockout + rr::KnockoutModel::Phase, &s.koPhase, 8);
                        TargetProcess.AddScatterReadRequest(sc, s.knockout + rr::KnockoutModel::EndTs, &s.koEndTsW, 8);
                        TargetProcess.AddScatterReadRequest(sc, s.knockout + rr::KnockoutModel::IsSurrendered, &s.koSurrendered, 1);
                        TargetProcess.AddScatterReadRequest(sc, s.knockout + rr::KnockoutModel::IsHandcuffing, &s.koHandcuffing, 1);
                        TargetProcess.AddScatterReadRequest(sc, s.knockout + rr::KnockoutModel::IsHandcuffed, &s.koHandcuffed, 1);
                    }
                    if (s.itemInHands > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.itemInHands + rr::ItemInHandsModel::Current, &s.weaponCurrent, 8);
                }
                if (!TargetProcess.ExecuteScatterReadCached(sc))
                    DLOG("ScatterFail: detail phase 2 (seCount=%d)", seCount);
                SwitchToThread();

                // Phase 3: third-level pointers + block reads
                for (int i = 0; i < seCount; ++i) {
                    auto& s = se[i];
                    if (s.healthModel > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.healthModel + rr::HealthModel::Health, &s.hb, sizeof(s.hb));
                    if (s.staminaModel > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.staminaModel + rr::StaminaModel::Amount, &s.staminaW, 8);
                    if (s.initArmorModel > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.initArmorModel + rr::InitArmorModel::Amount, &s.armorW, 8);
                    if (s.publicData > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.publicData + 0x38, &s.pubHasArmorW, 8);
                    if (s.common > 0x10000) {
                        TargetProcess.AddScatterReadRequest(sc, s.common + rr::CommonModel::Role, &s.commonRole, 8);
                        TargetProcess.AddScatterReadRequest(sc, s.common + rr::CommonModel::IsFiring, &s.firingW, 8);
                        TargetProcess.AddScatterReadRequest(sc, s.common + rr::CommonModel::IsSprinting, &s.sprintingW, 8);
                        TargetProcess.AddScatterReadRequest(sc, s.common + rr::CommonModel::IsCrouching, &s.crouchingW, 8);
                        TargetProcess.AddScatterReadRequest(sc, s.common + rr::CommonModel::IsDead, &s.deadCommonW, 8);
                        TargetProcess.AddScatterReadRequest(sc, s.common + rr::CommonModel::VehicleEntityId, &s.vehIdW, 8);
                    }
                    if (s.reloadState > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.reloadState + rr::ReferenceData::Value, &s.reloadDesc, 8);
                    if (s.koPhase > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.koPhase + rr::ReferenceData::Value, &s.koPhaseDesc, 8);
                    if (s.weaponCurrent > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.weaponCurrent + rr::EquipItemModel::Data, &s.weaponData, 8);
                    if (s.costModel > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.costModel + rr::RobberInventoryCostModel::CharacterInventory, &s.costs, sizeof(s.costs));
                }
                if (!TargetProcess.ExecuteScatterReadCached(sc))
                    DLOG("ScatterFail: detail phase 3 (seCount=%d)", seCount);
                SwitchToThread();

                // Phase 4: wrapper primitive values (all at +0x18)
                constexpr std::uintptr_t V = rr::PrimitiveData::value;
                for (int i = 0; i < seCount; ++i) {
                    auto& s = se[i];
                    if (s.hb.hw > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.hb.hw + V, &s.healthVal, 4);
                    if (s.isDeadWrapper > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.isDeadWrapper + V, &s.deadVal, 1);
                    if (s.staminaW > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.staminaW + V, &s.staminaVal, 4);
                    if (s.armorW > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.armorW + V, &s.armorVal, 4);
                    if (s.hasArmorWrapper > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.hasArmorWrapper + V, &s.hasArmorVal, 1);
                    if (s.pubHasArmorW > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.pubHasArmorW + 0x10, &s.pubHasArmorVal, 1);
                    if (s.commonRole > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.commonRole + rr::ReferenceData::Value, &s.roleDesc, 8);
                    if (s.firingW > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.firingW + V, &s.firingVal, 1);
                    if (s.sprintingW > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.sprintingW + V, &s.sprintingVal, 1);
                    if (s.crouchingW > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.crouchingW + V, &s.crouchingVal, 1);
                    if (s.deadCommonW > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.deadCommonW + V, &s.deadCommonVal, 1);
                    if (s.vehIdW > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.vehIdW + V, &s.vehIdCommon, 4);
                    if (s.vehicleWrapper > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.vehicleWrapper + V, &s.vehIdRole, 4);
                    if (s.isBgReloadWrapper > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.isBgReloadWrapper + V, &s.bgReloadVal, 1);
                    if (s.reloadEndTsW > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.reloadEndTsW + 0x18, &s.reloadEndTs, 8);
                    if (s.kdEndTsW > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.kdEndTsW + 0x18, &s.kdEndTs, 8);
                    if (s.kdLeaveModel > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.kdLeaveModel + rr::KnockdownLeaveStateModel::IsActive, &s.kdLeaveActive, 1);
                    if (s.koEndTsW > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.koEndTsW + 0x18, &s.koEndTs, 8);
                    if (s.weaponData > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.weaponData + rr::ReferenceData::Value, &s.weaponDesc, 8);
                }
                if (!TargetProcess.ExecuteScatterReadCached(sc))
                    DLOG("ScatterFail: detail phase 4 (seCount=%d)", seCount);
            }

            // Distribute scatter results into detail cache
            constexpr std::int64_t kSentinelTs = 1767225600000LL;
            for (int i = 0; i < seCount; ++i) {
                auto& s = se[i];
                auto& det = detailCache[s.entityId];

                // isDead (top-level wrapper)
                if (s.isDeadWrapper > 0x10000) det.isDead = s.deadVal != 0;

                // Health
                if (s.hb.hw > 0x10000 &&
                    std::isfinite(s.healthVal) && s.healthVal >= 0.f && s.healthVal <= 100000.f) {
                    det.health = s.healthVal;
                    det.healthKnown = true;
                }
                if (s.hb.maxAmt > 0 && s.hb.maxAmt <= 100000)
                    det.maxHealth = static_cast<float>(s.hb.maxAmt);
                if (s.hb.critHp >= 0 && s.hb.critHp <= 100000)
                    det.criticalHealth = static_cast<float>(s.hb.critHp);

                // Stamina / Armor
                if (s.staminaW > 0x10000) det.stamina = static_cast<float>(s.staminaVal);
                if (s.armorW > 0x10000) {
                    det.armor = static_cast<float>(s.armorVal);
                    if (s.armorVal > 0) det.hasArmor = true;
                }

                // Common state flags (overrides armor hasArmor)
                if (s.firingW > 0x10000) det.isFiring = s.firingVal != 0;
                if (s.sprintingW > 0x10000) det.isSprinting = s.sprintingVal != 0;
                if (s.crouchingW > 0x10000) det.isCrouching = s.crouchingVal != 0;
                if (s.deadCommonW > 0x10000 && s.deadCommonVal != 0) det.isDead = true;
                if (s.hasArmorWrapper > 0x10000) det.hasArmor = s.hasArmorVal != 0;
                // PublicData hasArmor fallback (Data+0x30 → +0x38 → +0x10)
                if (!det.hasArmor && s.pubHasArmorW > 0x10000)
                    det.hasArmor = s.pubHasArmorVal != 0;

                // Vehicle (from ReadRoleAndVehicle path)
                if (s.vehicleWrapper > 0x10000 && s.vehIdRole > 0) {
                    det.inVehicle = true;
                    det.vehicleId = s.vehIdRole;
                }
                // Vehicle (from ReadCommonState path, takes priority)
                if (s.vehIdW > 0x10000) {
                    det.inVehicle = s.vehIdCommon > 0;
                    det.vehicleId = s.vehIdCommon > 0 ? s.vehIdCommon : 0;
                }

                // Role (sequential — uses cache, usually hits)
                EntityData roleScratch{};
                ApplyRoleFromWrapper(s.playerRole, roleScratch);
                if (roleScratch.roleName.empty() && roleScratch.role == 0)
                    ApplyRoleFromWrapper(s.commonRole, roleScratch);
                // Direct offset fallback: commonRole+0x18 → desc+0x10 → name string
                if (roleScratch.roleName.empty() && roleScratch.role == 0 &&
                    s.roleDesc > 0x10000 && s.roleDesc < 0x7FFFFFFFFFFFull) {
                    const std::uintptr_t nameStr = ReadPtr(s.roleDesc + rr::Description::Name);
                    if (nameStr > 0x10000) {
                        std::wstring w;
                        if (RobRunMemory::ReadString(nameStr, w, 64) && !w.empty()) {
                            const std::string norm = NormalizeRoleName(RobRunMemory::WideToUtf8(w));
                            if (!norm.empty()) roleScratch.roleName = norm;
                        }
                    }
                }
                if (roleScratch.role != 0) det.role = roleScratch.role;
                if (!roleScratch.roleName.empty()) det.roleName = std::move(roleScratch.roleName);

                // Reload
                det.isReloading = false;
                det.reloadPhase.clear();
                if (s.reloadDesc > 0x10000) {
                    det.isReloading = true;
                    const std::uintptr_t nameStr = ReadPtr(s.reloadDesc + rr::Description::Name);
                    if (nameStr > 0x10000) {
                        std::wstring w;
                        RobRunMemory::ReadString(nameStr, w);
                        det.reloadPhase = RobRunMemory::WideToUtf8(w);
                    }
                }
                if (!det.isReloading && battleNowMs > 0 &&
                    s.reloadEndTs > kSentinelTs && s.reloadEndTs > battleNowMs)
                    det.isReloading = true;
                if (!det.isReloading && s.bgReloadVal != 0)
                    det.isReloading = true;

                // Knockdown (bounded remaining window to reject INT64_MAX/garbage sentinels)
                det.isKnockedDown = false;
                if (battleNowMs > 0 && s.kdEndTs > kSentinelTs && s.kdEndTs > battleNowMs &&
                    (s.kdEndTs - battleNowMs) <= kMaxKnockdownRemainMs &&
                    s.kdLeaveActive == 0)
                    det.isKnockedDown = true;

                // Knockout (validate phase description, bound EndTs, and exclude active cuffing)
                det.isKnockedOut = false;
                if (s.koHandcuffing == 0) {
                    if (IsActiveKnockoutPhaseDesc(s.koPhaseDesc))
                        det.isKnockedOut = true;
                    else if (battleNowMs > 0 && s.koEndTs > kSentinelTs && s.koEndTs > battleNowMs &&
                        (s.koEndTs - battleNowMs) <= kMaxKnockoutRemainMs)
                        det.isKnockedOut = true;
                }
                det.isSurrendered = s.koSurrendered != 0;
                det.isHandcuffed = s.koHandcuffed != 0;

                // A player actively sprinting, firing, reloading, or inside a vehicle
                // cannot be knocked down or knocked out.
                if (det.isSprinting || det.isFiring || det.isReloading || det.inVehicle) {
                    det.isKnockedDown = false;
                    det.isKnockedOut = false;
                }

                if (det.isKnockedOut && s.entityId == localEntityId &&
                    ConfigInstance.Aim.IgnoreKnock && s.knockout > 0x10000) {
                    const std::uint64_t nowMs = GetTickCount64();
                    if (sessionChangedMs == 0 || (nowMs >= sessionChangedMs && nowMs - sessionChangedMs >= 2000)) {
                        std::uintptr_t zero = 0;
                        TargetProcess.Write(
                            s.knockout + rr::KnockoutModel::Phase, zero);
                    }
                }

                // Weapon (uses cache — only reads strings on miss)
                if (s.weaponDesc > 0x10000) {
                    const auto wit = g_weaponDescCache.find(s.weaponDesc);
                    if (wit != g_weaponDescCache.end() && wit->second.valid) {
                        if (det.weaponName != wit->second.name) {
                            det.weaponName = wit->second.name;
                            det.weaponNameUtf8 = RobRunMemory::WideToUtf8(det.weaponName);
                        }
                        det.weaponIndex = wit->second.index;
                    } else {
                        const std::uintptr_t nameStr = ReadPtr(s.weaponDesc + rr::Description::Name);
                        std::wstring wn;
                        if (nameStr > 0x10000) RobRunMemory::ReadString(nameStr, wn);
                        if (!wn.empty()) {
                            std::wstring cleaned = CleanWeaponName(wn);
                            if (!cleaned.empty()) wn = std::move(cleaned);
                        }
                        const std::int32_t idx = RobRunMemory::ReadInt32(
                            s.weaponDesc + rr::InventoryItemDescription::Index);
                        det.weaponName = wn;
                        det.weaponNameUtf8 = RobRunMemory::WideToUtf8(det.weaponName);
                        det.weaponIndex = idx >= 0 ? idx : -1;
                        if (!wn.empty() || idx >= 0)
                            g_weaponDescCache[s.weaponDesc] = WeaponDescCache{ wn, det.weaponIndex, true };
                    }
                } else {
                    det.weaponName.clear();
                    det.weaponNameUtf8.clear();
                    det.weaponIndex = -1;
                }

                // Slot identity (sequential, cached)
                if (det.publicId == 0 || det.nickname.empty()) {
                    const auto slotIt = slots.find(s.entityId);
                    if (slotIt != slots.end()) {
                        EntityData slotScratch{};
                        ReadSlotIdentity(slotIt->second, slotScratch);
                        det.nickname = std::move(slotScratch.nickname);
                        det.clanTag = std::move(slotScratch.clanTag);
                        det.nicknameUtf8 = RobRunMemory::WideToUtf8(det.nickname);
                        det.clanTagUtf8 = RobRunMemory::WideToUtf8(det.clanTag);
                        det.publicId = slotScratch.publicId;
                    }
                }

                // Cost block (already scatter-read)
                if (s.costModel > 0x10000) {
                    det.bagValue = s.costs.charInv;
                    det.bagCash = s.costs.charInvCash;
                    det.bagItems = s.costs.charInvItems;
                    det.equipValue = s.costs.charEquip;
                }
            }
        }

        const std::uint64_t tAfterDet = SteadyMicros();
        tDetUs = tAfterDet - tAfterP3;

        // Hoist per-pass invariants outside the entity loop to avoid redundant
        // mutex locks, heap allocations, and matrix work when Chams are off.
        const bool needChamsMatrices = ConfigInstance.Player.Chams;
        const bool wantVisCheck = ConfigInstance.Player.VisCheck ||
            ConfigInstance.Player.WallCheck || ConfigInstance.Aim.VisibleBonesOnly;
        const bool wantOffscreenArrows = ConfigInstance.Player.OffscreenArrows;
        const float visMaxDist = ConfigInstance.Player.VisMaxDistance;
        const int visRayCount = ConfigInstance.Player.VisRayCount;
        const bool haveLocalPosPass = RobRunMemory::Finite(localPosition) && !localPosition.IsZero();
        const auto passBvh = wantVisCheck ? LoadBvhSnapshot() : nullptr;
        const bool passBvhUsable = passBvh && passBvh->valid && IsBvhActiveForCurrentLocation();
        RobRunCamera::CameraState liveVisCam = visCamera;
        if (wantVisCheck && passBvhUsable && !fullMapFastMode) {
            RobRunCamera::CameraState freshNow{};
            if (GetFreshCamera(freshNow) && freshNow.valid)
                liveVisCam = freshNow;
        }
        static std::vector<Skeleton::BoneSegment> s_tmpBones;
        static std::vector<Skeleton::BoneLocal> s_tmpLocal;
        static std::vector<Matrix4x4> s_tmpWorld;

        int fallbackBudget = 1;
        for (auto& w : work) {
            if (!w.positionValid) continue;
            if (uiCount <= 3) DLOG("UpdateInternal #%d: entity id=%d, cp=A (init)", uiCount, w.entityId);

            EntityData entity{};
            entity.entityId = w.entityId;
            entity.position = w.block.position;
            entity.characterModel = w.characterModel;
            if (std::isfinite(w.block.pitch)) entity.pitch = w.block.pitch;
            if (std::isfinite(w.block.yaw)) {
                entity.yaw = w.block.yaw;
                const float cosPitch = std::cos(w.block.pitch);
                entity.lookDirection = Vector3(
                    std::sin(w.block.yaw) * cosPitch,
                    std::sin(w.block.pitch),
                    std::cos(w.block.yaw) * cosPitch);
            }

            if (uiCount <= 3) DLOG("UpdateInternal #%d: cp=B (detail cache)", uiCount);
            auto& detail = detailCache[entity.entityId];

            entity.isDead = detail.isDead;
            entity.health = detail.health;
            entity.maxHealth = detail.maxHealth;
            entity.criticalHealth = detail.criticalHealth;
            entity.healthKnown = detail.healthKnown;
            entity.isSprinting = detail.isSprinting;
            entity.isCrouching = detail.isCrouching;
            entity.role = detail.role;
            entity.roleName = detail.roleName;
            entity.inVehicle = detail.inVehicle;
            entity.vehicleId = detail.vehicleId;
            entity.nickname = detail.nickname;
            entity.clanTag = detail.clanTag;
            entity.publicId = detail.publicId;
            entity.isPartyMember = IsPartyMember(entity.entityId, entity.publicId);
            entity.weaponName = detail.weaponName;
            // UTF-8 strings are cached inside detailCache when strings change.
            entity.nicknameUtf8 = detail.nicknameUtf8;
            entity.clanTagUtf8 = detail.clanTagUtf8;
            entity.weaponNameUtf8 = detail.weaponNameUtf8;
            entity.weaponIndex = detail.weaponIndex;
            entity.isFiring = detail.isFiring;
            entity.isReloading = detail.isReloading;
            entity.hasArmor = detail.hasArmor;
            entity.armor = detail.armor;
            entity.stamina = detail.stamina;
            entity.bagValue = detail.bagValue;
            entity.bagCash = detail.bagCash;
            entity.bagItems = detail.bagItems;
            entity.equipValue = detail.equipValue;
            entity.isKnockedDown = detail.isKnockedDown;
            entity.isKnockedOut = detail.isKnockedOut;
            entity.isSurrendered = detail.isSurrendered;
            entity.isHandcuffed = detail.isHandcuffed;
            entity.reloadPhase = detail.reloadPhase;

            if (uiCount <= 3) DLOG("UpdateInternal #%d: cp=C (detail done)", uiCount);

            const auto viewIt = views.find(w.entityId);
            if (viewIt != views.end())
                entity.viewPtr = viewIt->second;

            auto& rig = rigCache[w.entityId];

            if (uiCount <= 3) DLOG("UpdateInternal #%d: cp=D (rig, needsPose=%d, layoutValid=%d)", uiCount, (int)w.needsPose, (int)rig.layoutValid);

            try {
            if (w.needsPose) {
                static bool loggedPose = false;
                if (!loggedPose) {
                    DLOG("RobRunReader: needsPose, layout.count=%d, poseOffset=%zu, poseSize=%zu",
                        rig.layout.count, (size_t)w.poseOffset, (size_t)w.poseSize);
                }

                // Sample into reused scratch buffers so a failed read never
                // overwrites the last good frame and avoids per-entity allocations.
                s_tmpBones.clear();
                Skeleton::BonePoints tmpPoints{};
                s_tmpLocal.assign(static_cast<std::size_t>(rig.layout.count), Skeleton::BoneLocal{});
                if (needChamsMatrices)
                    s_tmpWorld.assign(static_cast<std::size_t>(rig.layout.count), Matrix4x4{});
                else
                    s_tmpWorld.clear();

                Skeleton::PoseStats pst{};
                bool sampled = Skeleton::ProcessPoseData(rig.layout,
                    poseBuf.data() + w.poseOffset, w.poseSize,
                    s_tmpBones, &tmpPoints, s_tmpLocal.data(),
                    needChamsMatrices ? s_tmpWorld.data() : nullptr, &pst);

                if (sampled) {
                    ++poseScatterOkAcc;
                } else {
                    ++poseScatterFailAcc;
                    pst = {};
                    s_tmpBones.clear();
                    s_tmpLocal.assign(static_cast<std::size_t>(rig.layout.count), Skeleton::BoneLocal{});
                    if (needChamsMatrices)
                        s_tmpWorld.assign(static_cast<std::size_t>(rig.layout.count), Matrix4x4{});
                    sampled = Skeleton::SampleLayout(rig.layout, DmaReadFresh,
                        s_tmpBones, &tmpPoints, s_tmpLocal.data(),
                        needChamsMatrices ? s_tmpWorld.data() : nullptr, &pst);
                    if (sampled) ++poseFreshOkAcc;
                }
                boneTotalAcc += pst.total;
                boneInvalidAcc += pst.invalid;
                boneNormZeroAcc += pst.normZero;
                boneNormBadAcc += pst.normBad;
                bonePosInfAcc += pst.posInf;
                boneChainDropAcc += pst.chainDrop;
                if (!loggedPose) {
                    DLOG("RobRunReader: pose sampled=%d, worldMatrices.size=%zu",
                        sampled, rig.worldMatrices.size());
                    loggedPose = true;
                }

                if (!sampled) {
                    if (rig.poseFailures == 0)
                        rig.poseFailureSince = now;
                    ++rig.poseFailures;
                    ++poseFailureCount;
                    // Failed read: keep drawing from cached bones.
                    // Only drop layout after sustained failure.
                    if (!Skeleton::ValidateHierarchy(rig.layout, DmaReadFresh) ||
                        now - rig.poseFailureSince >= 350) {
                        rig.layoutValid = false;
                        rig.layoutStamp = 0;
                        rig.poseFailures = 0;
                    }
                    if (now - rig.poseFailureSince >= 2500) {
                        rig.bones.clear();
                        rig.points = Skeleton::BonePoints{};
                        rig.smoothedValid = false;
                        rig.smoothedWorldMatrices.clear();
                        rig.poseHistCount = 0;
                        rig.poseHistCursor = 0;
                    }
                } else {
                    // Pelvis-relative 3-sample temporal median:
                    // anchor is taken from the SAME TransformHierarchy read so
                    // world-space movement has 0 ms lag while any isolated
                    // 1-frame pre-LateUpdate / torn-DMA flicker is erased.
                    const Skeleton::Vec anchor = tmpPoints.hasPelvis
                        ? tmpPoints.pelvis
                        : (tmpPoints.hasChest
                            ? tmpPoints.chest
                            : Skeleton::Vec{ entity.position.x, entity.position.y, entity.position.z });

                    if (rig.poseHistCount > 0 &&
                        rig.relBoneHist[(rig.poseHistCursor + 2) % 3].size() != s_tmpBones.size())
                    {
                        rig.poseHistCount = 0;
                        rig.poseHistCursor = 0;
                    }

                    const int slot = rig.poseHistCursor;
                    auto& relSegs = rig.relBoneHist[slot];
                    relSegs.resize(s_tmpBones.size());
                    for (std::size_t i = 0; i < s_tmpBones.size(); ++i) {
                        relSegs[i].a = { s_tmpBones[i].a.x - anchor.x, s_tmpBones[i].a.y - anchor.y, s_tmpBones[i].a.z - anchor.z };
                        relSegs[i].b = { s_tmpBones[i].b.x - anchor.x, s_tmpBones[i].b.y - anchor.y, s_tmpBones[i].b.z - anchor.z };
                        relSegs[i].visible = s_tmpBones[i].visible;
                    }
                    auto subVec = [&](const Skeleton::Vec& v) -> Skeleton::Vec {
                        return { v.x - anchor.x, v.y - anchor.y, v.z - anchor.z };
                    };
                    auto& relPts = rig.relPtsHist[slot];
                    relPts = tmpPoints;
                    relPts.head          = subVec(tmpPoints.head);
                    relPts.neck          = subVec(tmpPoints.neck);
                    relPts.chest         = subVec(tmpPoints.chest);
                    relPts.pelvis        = subVec(tmpPoints.pelvis);
                    relPts.leftShoulder  = subVec(tmpPoints.leftShoulder);
                    relPts.leftElbow     = subVec(tmpPoints.leftElbow);
                    relPts.leftHand      = subVec(tmpPoints.leftHand);
                    relPts.rightShoulder = subVec(tmpPoints.rightShoulder);
                    relPts.rightElbow    = subVec(tmpPoints.rightElbow);
                    relPts.rightHand     = subVec(tmpPoints.rightHand);
                    relPts.leftHip       = subVec(tmpPoints.leftHip);
                    relPts.leftKnee      = subVec(tmpPoints.leftKnee);
                    relPts.leftFoot      = subVec(tmpPoints.leftFoot);
                    relPts.rightHip      = subVec(tmpPoints.rightHip);
                    relPts.rightKnee     = subVec(tmpPoints.rightKnee);
                    relPts.rightFoot     = subVec(tmpPoints.rightFoot);

                    rig.poseHistCursor = (slot + 1) % 3;
                    if (rig.poseHistCount < 3) ++rig.poseHistCount;

                    if (rig.poseHistCount == 3) {
                        auto med3f = [](float a, float b, float c) -> float {
                            return (std::max)((std::min)(a, b), (std::min)((std::max)(a, b), c));
                        };
                        auto med3v = [&](const Skeleton::Vec& v0, const Skeleton::Vec& v1, const Skeleton::Vec& v2) -> Skeleton::Vec {
                            return {
                                anchor.x + med3f(v0.x, v1.x, v2.x),
                                anchor.y + med3f(v0.y, v1.y, v2.y),
                                anchor.z + med3f(v0.z, v1.z, v2.z)
                            };
                        };
                        const auto& h0 = rig.relBoneHist[0];
                        const auto& h1 = rig.relBoneHist[1];
                        const auto& h2 = rig.relBoneHist[2];
                        for (std::size_t i = 0; i < s_tmpBones.size(); ++i) {
                            s_tmpBones[i].a = med3v(h0[i].a, h1[i].a, h2[i].a);
                            s_tmpBones[i].b = med3v(h0[i].b, h1[i].b, h2[i].b);
                        }
                        const auto& p0 = rig.relPtsHist[0];
                        const auto& p1 = rig.relPtsHist[1];
                        const auto& p2 = rig.relPtsHist[2];
                        tmpPoints.head          = med3v(p0.head, p1.head, p2.head);
                        tmpPoints.neck          = med3v(p0.neck, p1.neck, p2.neck);
                        tmpPoints.chest         = med3v(p0.chest, p1.chest, p2.chest);
                        tmpPoints.pelvis        = med3v(p0.pelvis, p1.pelvis, p2.pelvis);
                        tmpPoints.leftShoulder  = med3v(p0.leftShoulder, p1.leftShoulder, p2.leftShoulder);
                        tmpPoints.leftElbow     = med3v(p0.leftElbow, p1.leftElbow, p2.leftElbow);
                        tmpPoints.leftHand      = med3v(p0.leftHand, p1.leftHand, p2.leftHand);
                        tmpPoints.rightShoulder = med3v(p0.rightShoulder, p1.rightShoulder, p2.rightShoulder);
                        tmpPoints.rightElbow    = med3v(p0.rightElbow, p1.rightElbow, p2.rightElbow);
                        tmpPoints.rightHand     = med3v(p0.rightHand, p1.rightHand, p2.rightHand);
                        tmpPoints.leftHip       = med3v(p0.leftHip, p1.leftHip, p2.leftHip);
                        tmpPoints.leftKnee      = med3v(p0.leftKnee, p1.leftKnee, p2.leftKnee);
                        tmpPoints.leftFoot      = med3v(p0.leftFoot, p1.leftFoot, p2.leftFoot);
                        tmpPoints.rightHip      = med3v(p0.rightHip, p1.rightHip, p2.rightHip);
                        tmpPoints.rightKnee     = med3v(p0.rightKnee, p1.rightKnee, p2.rightKnee);
                        tmpPoints.rightFoot     = med3v(p0.rightFoot, p1.rightFoot, p2.rightFoot);
                    }

                    // Commit every pass so world position stays at full worker rate.
                    rig.bones.swap(s_tmpBones);
                    rig.points = tmpPoints;
                    rig.curLocal.swap(s_tmpLocal);
                    if (needChamsMatrices) {
                        rig.worldMatrices.swap(s_tmpWorld);
                    } else {
                        rig.worldMatrices.clear();
                        rig.smoothedWorldMatrices.clear();
                    }
                    rig.poseFailures = 0;
                    rig.poseFailureSince = 0;
                    constexpr float kPosEpsSq  = 1.0e-6f;
                    constexpr float kRotEps    = 1.0e-6f;
                    constexpr float kScaleEps  = 1.0e-6f;
                    constexpr std::uint64_t kFreezeMs = 500;

                    bool moved = !rig.prevLocalValid ||
                        rig.prevLocal.size() != rig.curLocal.size();
                    if (!moved) {
                        for (std::size_t b = 0; b < rig.curLocal.size(); ++b) {
                            const auto& c = rig.curLocal[b];
                            const auto& p = rig.prevLocal[b];

                            const float dx = c.position.x - p.position.x;
                            const float dy = c.position.y - p.position.y;
                            const float dz = c.position.z - p.position.z;
                            if (dx*dx + dy*dy + dz*dz > kPosEpsSq) { moved = true; break; }

                            const float dot =
                                c.rotation[0]*p.rotation[0] +
                                c.rotation[1]*p.rotation[1] +
                                c.rotation[2]*p.rotation[2] +
                                c.rotation[3]*p.rotation[3];
                            if (std::fabs(dot) < 1.0f - kRotEps) { moved = true; break; }

                            if (std::fabs(c.scale.x - p.scale.x) > kScaleEps ||
                                std::fabs(c.scale.y - p.scale.y) > kScaleEps ||
                                std::fabs(c.scale.z - p.scale.z) > kScaleEps) {
                                moved = true;
                                break;
                            }
                        }
                    }

                    if (moved || entity.inVehicle)
                        rig.lastMotionTick = now;
                    rig.animatorActive = rig.lastMotionTick == 0 ||
                        now - rig.lastMotionTick < kFreezeMs || entity.inVehicle;

                    // Default visibility is animator state. It is only a
                    // fallback: when the occluder scene is loaded the BVH pass
                    // below replaces it with true camera occlusion.
                    for (auto& seg : rig.bones)
                        seg.visible = rig.animatorActive;

                    rig.prevLocal.swap(rig.curLocal);
                    rig.prevLocalValid = true;

                    // Velocity-adaptive smoothing: smoothly scale alpha with movement distance
                    // to prevent binary toggle chatter while snapping on high-speed displacement / vehicles.
                    const float moveDist = Distance(rig.lastSampledPos, entity.position);
                    rig.lastSampledPos = entity.position;
                    const bool snapPose = entity.inVehicle || (moveDist > 0.40f);

                    // Exponential smoothing on the worker. PoseSmoothAlpha is
                    // the base weight of the new sample: 1 publishes the raw pose,
                    // lower values damp the per-frame jitter that partial or
                    // stale DMA reads add to the sample.
                    if (!snapPose && ConfigInstance.Player.PoseSmoothing && rig.smoothedValid &&
                        rig.smoothedBones.size() == rig.bones.size() &&
                        (rig.worldMatrices.empty() || rig.smoothedWorldMatrices.size() == rig.worldMatrices.size())) {
                        const float baseAlpha = std::clamp(ConfigInstance.Player.PoseSmoothAlpha, 0.10f, 1.0f);
                        const float alpha = std::clamp(baseAlpha + (moveDist / 0.15f) * (1.0f - baseAlpha), 0.15f, 1.0f);
                        const float inv = 1.0f - alpha;
                        for (std::size_t i = 0; i < rig.bones.size(); ++i) {
                            auto& s = rig.smoothedBones[i];
                            const auto& r = rig.bones[i];
                            const float dax = r.a.x-s.a.x, day = r.a.y-s.a.y, daz = r.a.z-s.a.z;
                            const float dbx = r.b.x-s.b.x, dby = r.b.y-s.b.y, dbz = r.b.z-s.b.z;
                            if (dax*dax+day*day+daz*daz > 0.25f ||
                                dbx*dbx+dby*dby+dbz*dbz > 0.25f) {
                                s.a = r.a;
                                s.b = r.b;
                                s.visible = r.visible;
                                continue;
                            }
                            s.a.x = s.a.x * inv + r.a.x * alpha;
                            s.a.y = s.a.y * inv + r.a.y * alpha;
                            s.a.z = s.a.z * inv + r.a.z * alpha;
                            s.b.x = s.b.x * inv + r.b.x * alpha;
                            s.b.y = s.b.y * inv + r.b.y * alpha;
                            s.b.z = s.b.z * inv + r.b.z * alpha;
                            s.visible = r.visible;
                        }
                        auto lerpVec = [&](Skeleton::Vec& dst, const Skeleton::Vec& src) {
                            const float dx = src.x-dst.x, dy = src.y-dst.y, dz = src.z-dst.z;
                            if (dx*dx+dy*dy+dz*dz > 0.25f) {
                                dst = src;
                                return;
                            }
                            dst.x = dst.x * inv + src.x * alpha;
                            dst.y = dst.y * inv + src.y * alpha;
                            dst.z = dst.z * inv + src.z * alpha;
                        };
                        rig.smoothedPoints.valid = rig.points.valid;
                        rig.smoothedPoints.hasHead = rig.points.hasHead;
                        rig.smoothedPoints.hasNeck = rig.points.hasNeck;
                        rig.smoothedPoints.hasChest = rig.points.hasChest;
                        rig.smoothedPoints.hasPelvis = rig.points.hasPelvis;
                        lerpVec(rig.smoothedPoints.head, rig.points.head);
                        lerpVec(rig.smoothedPoints.neck, rig.points.neck);
                        lerpVec(rig.smoothedPoints.chest, rig.points.chest);
                        lerpVec(rig.smoothedPoints.pelvis, rig.points.pelvis);
                        lerpVec(rig.smoothedPoints.leftShoulder, rig.points.leftShoulder);
                        lerpVec(rig.smoothedPoints.leftElbow, rig.points.leftElbow);
                        lerpVec(rig.smoothedPoints.leftHand, rig.points.leftHand);
                        lerpVec(rig.smoothedPoints.rightShoulder, rig.points.rightShoulder);
                        lerpVec(rig.smoothedPoints.rightElbow, rig.points.rightElbow);
                        lerpVec(rig.smoothedPoints.rightHand, rig.points.rightHand);
                        lerpVec(rig.smoothedPoints.leftHip, rig.points.leftHip);
                        lerpVec(rig.smoothedPoints.leftKnee, rig.points.leftKnee);
                        lerpVec(rig.smoothedPoints.leftFoot, rig.points.leftFoot);
                        lerpVec(rig.smoothedPoints.rightHip, rig.points.rightHip);
                        lerpVec(rig.smoothedPoints.rightKnee, rig.points.rightKnee);
                        lerpVec(rig.smoothedPoints.rightFoot, rig.points.rightFoot);

                        if (needChamsMatrices) {
                            for (std::size_t i = 0; i < rig.worldMatrices.size(); ++i) {
                                auto& sm = rig.smoothedWorldMatrices[i];
                                const auto& rm = rig.worldMatrices[i];
                                const float dx = rm.matrix[3][0]-sm.matrix[3][0];
                                const float dy = rm.matrix[3][1]-sm.matrix[3][1];
                                const float dz = rm.matrix[3][2]-sm.matrix[3][2];
                                if (dx*dx+dy*dy+dz*dz > 0.25f) {
                                    sm = rm;
                                    continue;
                                }
                                for (int r = 0; r < 4; ++r) {
                                    for (int c = 0; c < 4; ++c) {
                                        sm.matrix[r][c] = sm.matrix[r][c] * inv + rm.matrix[r][c] * alpha;
                                    }
                                }
                            }
                        }
                    } else {
                        rig.smoothedBones = rig.bones;
                        rig.smoothedPoints = rig.points;
                        if (needChamsMatrices)
                            rig.smoothedWorldMatrices = rig.worldMatrices;
                        rig.smoothedValid = true;
                    }
                }
            } else if (!fullMapFastMode &&
                       !rig.layoutValid &&
                       entity.entityId != localEntityId &&
                       !entity.isDead &&
                       (!haveLocalPos || Distance(entity.position, localPosition) <= poseCullDist) &&
                       fallbackBudget > 0 &&
                       now - rig.fallbackStamp > 1000) {
                rig.fallbackStamp = now;
                --fallbackBudget;
                ++passFallbacks;
                // Promote unmatched entities straight into cachedViewOwner so
                // Phase 3 caches their RigLayout and samples them at full worker
                // rate instead of re-running BuildSkeleton at 1 Hz forever.
                const std::uintptr_t foundView = static_cast<std::uintptr_t>(
                    Skeleton::FindEntityView(cachedGame, cachedBattleModel, entity.entityId, DmaRead));
                if (foundView > 0x10000) {
                    cachedViewOwner[entity.entityId] = foundView;
                    rig.view = foundView;
                    rig.layoutValid = false;
                    rig.layoutStamp = 0;
                    rig.upgradeAttempts = 0;
                }
            }

            if (ConfigInstance.Player.PoseSmoothing && rig.smoothedValid) {
                entity.bones = rig.smoothedBones;
                entity.bonePoints = rig.smoothedPoints;
            } else {
                entity.bones = rig.bones;
                entity.bonePoints = rig.points;
            }
            // Cross-check knocked state against live skeleton posture: a character
            // standing upright (head >= 1.35 m above feet) and not surrendered/cuffed
            // is not lying knocked down/out (guards against stale/police KnockoutModel state).
            if ((entity.isKnockedDown || entity.isKnockedOut) &&
                !entity.isSurrendered && !entity.isHandcuffed &&
                entity.bonePoints.valid && entity.bonePoints.hasHead) {
                const float headH = entity.bonePoints.head.y - entity.position.y;
                if (std::isfinite(headH) && headH >= 1.35f && headH <= 2.25f) {
                    entity.isKnockedDown = false;
                    entity.isKnockedOut = false;
                }
            }
            } catch (...) {
                static int catchCount = 0;
                if (catchCount++ < 10) DLOG("UpdateInternal: skeleton catch(...) fired for entity %d", entity.entityId);
            }
            if (uiCount <= 3) DLOG("UpdateInternal #%d: cp=E (bones done)", uiCount);
            if (needChamsMatrices) {
                const auto& activeWorldMatrices = (ConfigInstance.Player.PoseSmoothing && rig.smoothedValid && !rig.smoothedWorldMatrices.empty())
                    ? rig.smoothedWorldMatrices
                    : rig.worldMatrices;
                if (!activeWorldMatrices.empty() &&
                    activeWorldMatrices.size() == static_cast<std::size_t>(rig.layout.count)) {
                    entity.boneWorldMatrices = activeWorldMatrices;
                    entity.boneCount = rig.layout.count;
                    entity.boneMap = rig.layout.boneMap;
                    static bool loggedCopy = false;
                    if (!loggedCopy) {
                        DLOG("RobRunReader: copied %zu worldMatrices to entity", activeWorldMatrices.size());
                        loggedCopy = true;
                    }
                }
            }
            entity.visible = rig.animatorActive;

            // Camera-occlusion pass. VisCheck colours each bone by whether the
            // camera can see it; WallCheck also hides the whole character from
            // the aim gate when every bone is occluded. Both need the BVH, so
            // without it the animator fallback above stays in effect and the
            // menu says the occluder file is missing.
            const float entDist = haveLocalPosPass ? Distance(entity.position, localPosition) : 0.f;
            if (visMaxDist > 0.f && entDist > visMaxDist) {
                if (!ConfigInstance.Aim.VisibleBonesOnly) {
                    entity.visible = false;
                    for (auto& seg : entity.bones) {
                        seg.visible = false;
                    }
                }
            }
            else if (!passBvhUsable && ConfigInstance.Aim.VisibleBonesOnly) {
                // No valid BVH for this map (e.g., Hub/Cottage) — don't falsely reject
                // standing-still players based on animator motion alone.
                entity.visible = true;
            }
            else if (!fullMapFastMode &&
                wantVisCheck &&
                passBvhUsable &&
                entity.entityId != localEntityId)
            {
                if (liveVisCam.valid) {
                const CollisionBVH::Float3 cam = {
                    liveVisCam.position.x,
                    liveVisCam.position.y,
                    liveVisCam.position.z };

                // Skip raycasting players well behind the camera unless offscreen
                // arrows need live visibility tinting.
                bool behindCamera = false;
                if (!wantOffscreenArrows && entDist > 3.0f) {
                    const float tx = entity.position.x - cam.x;
                    const float ty = (entity.position.y + 1.0f) - cam.y;
                    const float tz = entity.position.z - cam.z;
                    const float lenSq = tx * tx + ty * ty + tz * tz;
                    if (lenSq > 1.0f) {
                        const float fwdDot = (tx * liveVisCam.forward.x +
                                              ty * liveVisCam.forward.y +
                                              tz * liveVisCam.forward.z) / std::sqrt(lenSq);
                        behindCamera = (fwdDot < -0.25f);
                    }
                }

                if (behindCamera) {
                    entity.visible = false;
                    for (auto& seg : entity.bones)
                        seg.visible = false;
                } else {
                const auto& bvh = passBvh;
                bool anyVisible = false;
                const int rayCount = visRayCount;

                if (rayCount <= 2 || entity.bones.empty()) {
                    // Level 1 (or fallback when Skeleton ESP is off / bones empty):
                    // 2 Rays (Torso + Head). Never leaves anyVisible=false just because bones weren't built.
                    const CollisionBVH::Float3 torso = {
                        entity.position.x,
                        entity.position.y + 0.95f,
                        entity.position.z };
                    const CollisionBVH::Float3 head = {
                        entity.position.x,
                        entity.position.y + 1.65f,
                        entity.position.z };

                    bool torsoVis = !CollisionBVH::IsOccluded(*bvh, cam, torso, &dynCopy, carCabinSolid);
                    bool headVis = !CollisionBVH::IsOccluded(*bvh, cam, head, &dynCopy, carCabinSolid);
                    anyVisible = torsoVis || headVis;
                    for (auto& seg : entity.bones) {
                        seg.visible = anyVisible;
                    }
                } else if (rayCount <= 6) {
                    // Level 2: 6 Rays (Head, Chest, Pelvis, Left Hand, Right Hand, Feet)
                    const auto& bp = entity.bonePoints;
                    const CollisionBVH::Float3 pHead = bp.hasHead ?
                        CollisionBVH::Float3{ bp.head.x, bp.head.y, bp.head.z } :
                        CollisionBVH::Float3{ entity.position.x, entity.position.y + 1.70f, entity.position.z };
                    const CollisionBVH::Float3 pChest = bp.hasChest ?
                        CollisionBVH::Float3{ bp.chest.x, bp.chest.y, bp.chest.z } :
                        CollisionBVH::Float3{ entity.position.x, entity.position.y + 1.25f, entity.position.z };
                    const CollisionBVH::Float3 pPelvis = bp.hasPelvis ?
                        CollisionBVH::Float3{ bp.pelvis.x, bp.pelvis.y, bp.pelvis.z } :
                        CollisionBVH::Float3{ entity.position.x, entity.position.y + 0.85f, entity.position.z };
                    const CollisionBVH::Float3 pLHand = (std::abs(bp.leftHand.x) > 0.01f) ?
                        CollisionBVH::Float3{ bp.leftHand.x, bp.leftHand.y, bp.leftHand.z } :
                        CollisionBVH::Float3{ entity.position.x - 0.40f, entity.position.y + 1.0f, entity.position.z };
                    const CollisionBVH::Float3 pRHand = (std::abs(bp.rightHand.x) > 0.01f) ?
                        CollisionBVH::Float3{ bp.rightHand.x, bp.rightHand.y, bp.rightHand.z } :
                        CollisionBVH::Float3{ entity.position.x + 0.40f, entity.position.y + 1.0f, entity.position.z };
                    const CollisionBVH::Float3 pFeet = { entity.position.x, entity.position.y + 0.15f, entity.position.z };

                    bool vHead   = !CollisionBVH::IsOccluded(*bvh, cam, pHead, &dynCopy, carCabinSolid);
                    bool vChest  = !CollisionBVH::IsOccluded(*bvh, cam, pChest, &dynCopy, carCabinSolid);
                    bool vPelvis = !CollisionBVH::IsOccluded(*bvh, cam, pPelvis, &dynCopy, carCabinSolid);
                    bool vLHand  = !CollisionBVH::IsOccluded(*bvh, cam, pLHand, &dynCopy, carCabinSolid);
                    bool vRHand  = !CollisionBVH::IsOccluded(*bvh, cam, pRHand, &dynCopy, carCabinSolid);
                    bool vFeet   = !CollisionBVH::IsOccluded(*bvh, cam, pFeet, &dynCopy, carCabinSolid);

                    anyVisible = (vHead || vChest || vPelvis || vLHand || vRHand || vFeet);

                    for (auto& seg : entity.bones) {
                        const CollisionBVH::Float3 mid = {
                            (seg.a.x + seg.b.x) * 0.5f,
                            (seg.a.y + seg.b.y) * 0.5f,
                            (seg.a.z + seg.b.z) * 0.5f };
                        if (mid.y >= entity.position.y + 1.40f) seg.visible = vHead;
                        else if (mid.y >= entity.position.y + 1.05f) seg.visible = (vChest || vLHand || vRHand);
                        else if (mid.y >= entity.position.y + 0.65f) seg.visible = vPelvis;
                        else seg.visible = vFeet;
                    }
                } else {
                    // Level 3: 18+ Rays (Full Rig segment evaluation for exact per-limb highlights)
                    for (auto& seg : entity.bones) {
                        const CollisionBVH::Float3 mid = {
                            (seg.a.x + seg.b.x) * 0.5f,
                            (seg.a.y + seg.b.y) * 0.5f,
                            (seg.a.z + seg.b.z) * 0.5f };
                        seg.visible = !CollisionBVH::IsOccluded(*bvh, cam, mid, &dynCopy, carCabinSolid);
                        if (seg.visible)
                            anyVisible = true;
                    }
                }
                entity.visible = anyVisible;
                }
                }
            }

            if (entity.entityId == localEntityId) {
                if (localPosition.IsZero() && !entity.position.IsZero()) {
                    localPosition = entity.position;
                }
                if (std::isfinite(entity.yaw) && entity.yaw != 0.f) {
                    localYaw = entity.yaw;
                }
                if (std::isfinite(entity.pitch) && entity.pitch != 0.f) {
                    localPitch = entity.pitch;
                }
            }

            if (uiCount <= 3) DLOG("UpdateInternal #%d: cp=F (pre-push)", uiCount);
            entities.push_back(std::move(entity));
            if (uiCount <= 3) DLOG("UpdateInternal #%d: cp=G (entity done)", uiCount);
        }
        if (uiCount <= 3) DLOG("UpdateInternal #%d: entity loop done, %zu entities", uiCount, entities.size());

        // Mark clan mates: match each entity's clan tag against the local player's.
        std::wstring localClan;
        for (const auto& e : entities) {
            if (e.entityId == localEntityId && !e.clanTag.empty()) {
                localClan = e.clanTag;
                break;
            }
        }
        if (!localClan.empty()) {
            for (auto& e : entities) {
                if (e.entityId != localEntityId && e.clanTag == localClan)
                    e.isClanMate = true;
            }
        }
        const std::uint64_t tAfterP5 = SteadyMicros();
        tP5Us = tAfterP5 - tAfterDet;
    } else {
        if (++entityDictFailures >= 50) {
            cachedBattleEntities = 0;
            entityDictFailures = 0;
        }
    }

    // The view dictionary is refreshed at most every 400 ms. Match it against
    // this pass's fresh player positions so the pairing is ready for the next
    // passes; doing it before the entity loop would match against a list that
    // was just cleared.
    if (viewsRefreshed)
    {
        const std::uint64_t tMatchStart = SteadyMicros();
        viewsRefreshed = false;
        MatchViewsToEntities();
        tMatchUs = SteadyMicros() - tMatchStart;
    }

    const std::uint64_t passNow = GetTickCount64();

    // Opportunistic manual Mem/TLB cache refresh under -norefresh:
    // 1) When no enemy is currently rendered on screen, everyone on screen is
    //    far away (> 80 m), or the lobby is empty, refresh caches every 2.5 s
    //    so newly mapped pages stay warm with zero combat stutter.
    // 2) Safety net: if nearby living players exist yet most/all have no bones
    //    for >= 600 ms (e.g. spawned straight into a new area with stale TLB),
    //    force a cache/TLB refresh and immediately re-read views + layouts.
    {
        const std::uint64_t reportMs = g_renderReportMs.load(std::memory_order_relaxed);
        const bool reportFresh = reportMs != 0 && passNow >= reportMs && (passNow - reportMs) < 1500;
        const bool anyOnScreen = reportFresh && g_renderAnyOnScreen.load(std::memory_order_relaxed);
        const float nearestDist = reportFresh ? g_renderNearestDist.load(std::memory_order_relaxed) : 9999.f;

        if (!anyOnScreen || nearestDist > 80.f || entities.size() <= 1) {
            MaybeRefreshDmaCaches("idleOrOutOfSight", 2500);
        }

        const bool haveLocal = RobRunMemory::Finite(localPosition) && !localPosition.IsZero();
        const float boneRange = static_cast<float>(ConfigInstance.Player.MaxDistance);
        int nearbyLiving = 0;
        int nearbyWithBones = 0;
        for (const auto& e : entities) {
            if (e.entityId == localEntityId || e.isDead) continue;
            if (haveLocal && Distance(e.position, localPosition) > boneRange) continue;
            ++nearbyLiving;
            if (!e.bones.empty()) ++nearbyWithBones;
        }

        static std::uint64_t s_boneStarveSince = 0;
        if (nearbyLiving > 0 && (nearbyWithBones == 0 || nearbyWithBones * 2 < nearbyLiving)) {
            if (s_boneStarveSince == 0)
                s_boneStarveSince = passNow;
            if (passNow - s_boneStarveSince >= 600) {
                if (MaybeRefreshDmaCaches("boneStarvation", 2500)) {
                    viewRefreshStamp = 0;
                    for (auto& kv : rigCache) {
                        if (kv.second.bones.empty()) {
                            kv.second.layoutValid = false;
                            kv.second.layoutStamp = 0;
                        }
                    }
                }
            }
        } else {
            s_boneStarveSince = 0;
        }
        g_perfSkels.store(nearbyWithBones, std::memory_order_relaxed);
        g_perfLiving.store(nearbyLiving, std::memory_order_relaxed);
        g_perfP1Us.fetch_add(tP1Us, std::memory_order_relaxed);
        g_perfP2Us.fetch_add(tP2Us, std::memory_order_relaxed);
        g_perfP3Us.fetch_add(tP3Us, std::memory_order_relaxed);
        g_perfDetUs.fetch_add(tDetUs, std::memory_order_relaxed);
        g_perfP5Us.fetch_add(tP5Us, std::memory_order_relaxed);
        if (passRebuilds) g_perfRebuilds.fetch_add(static_cast<std::uint64_t>(passRebuilds), std::memory_order_relaxed);
        if (passFallbacks) g_perfFallbacks.fetch_add(static_cast<std::uint64_t>(passFallbacks), std::memory_order_relaxed);

        const std::uint64_t totalPassUs = SteadyMicros() - tPassStart;
        if (totalPassUs >= 50'000ULL) {
            static std::uint64_t s_lastSlowLogMs = 0;
            if (passNow - s_lastSlowLogMs >= 2000) {
                s_lastSlowLogMs = passNow;
                std::printf("[SlowPass] total=%.1fms (roots=%.1f views=%.1f p1=%.1f p2=%.1f p3=%.1f det=%.1f p5=%.1f match=%.1f) reb=%d fb=%d skel=%d/%d ents=%zu\n",
                    totalPassUs / 1000.0,
                    tRootsUs / 1000.0, tViewsUs / 1000.0,
                    tP1Us / 1000.0, tP2Us / 1000.0, tP3Us / 1000.0,
                    tDetUs / 1000.0, tP5Us / 1000.0, tMatchUs / 1000.0,
                    passRebuilds, passFallbacks, nearbyWithBones, nearbyLiving, entities.size());
            }
        }
    }

    // World and inventory reading runs on dedicated g_worldWorkerThread
    // so this player pose pass runs at full speed without latency spikes.

    if (cachedGame > 0x10000) {
        if (ConfigInstance.Aim.HideCrosshair)
            HideDefaultCrosshair(cachedGame);
        else
            RestoreDefaultCrosshair();
    }

    // Detect a dead/restarted game even if cached root pointers stayed non-zero
    // under -norefresh, WITHOUT calling RefreshProcessesFast() during active
    // gameplay (VMMDLL_OPT_REFRESH_FREQ_FAST stalls the DMA bus and causes
    // visible render/camera stutters).
    {
        const std::uint64_t nowMs = GetTickCount64();
        if (entities.empty()) {
            CheckGameProcessAlive();
        } else {
            auto mixU32 = [](std::uint64_t h, std::uint32_t v) -> std::uint64_t {
                return (h ^ v) * 1099511628211ULL;
            };
            auto fbits = [](float f) -> std::uint32_t {
                std::uint32_t u = 0;
                std::memcpy(&u, &f, sizeof(u));
                return u;
            };
            std::uint64_t h = 14695981039346656037ULL;
            h = mixU32(h, static_cast<std::uint32_t>(entities.size()));
            h = mixU32(h, fbits(cameraState.position.x));
            h = mixU32(h, fbits(cameraState.position.y));
            h = mixU32(h, fbits(cameraState.position.z));
            h = mixU32(h, fbits(cameraState.forward.x));
            h = mixU32(h, fbits(cameraState.forward.y));
            h = mixU32(h, fbits(cameraState.forward.z));
            for (const auto& e : entities) {
                h = mixU32(h, fbits(e.position.x));
                h = mixU32(h, fbits(e.position.y));
                h = mixU32(h, fbits(e.position.z));
            }
            if (h != g_lastLiveHash || g_lastLiveChangeMs == 0) {
                g_lastLiveHash = h;
                g_lastLiveChangeMs = nowMs;
            } else if (nowMs >= g_lastLiveChangeMs && (nowMs - g_lastLiveChangeMs) >= 5000) {
                // Camera and all entity positions have been bit-for-bit frozen
                // for 5s+: verify the game process is still alive.
                CheckGameProcessAlive();
            }
        }
    }

    // A root change here means this pass rebuilt the scene identity. Bump the
    // generation so memory aim blocks writes while the new objects settle.
    NoteSessionState();

    PruneCaches();
    failureCount = 0;
}

void Update()
{
    static int updateCount = 0;
    ++updateCount;
    if (updateCount <= 3 || (updateCount % 500) == 0)
        DLOG("Update #%d", updateCount);

    try {
        TryLoadBvh(false);

        UpdateInternal();
        PublishSnapshot();
    } catch (const std::exception& e) {
        DLOG("Update EXCEPTION #%d: %s", updateCount, e.what());
    } catch (...) {
        DLOG("Update SEH/UNKNOWN EXCEPTION #%d (AV caught by /EHa)", updateCount);
    }
}

SnapshotPtr GetSnapshot()
{
    std::lock_guard<std::mutex> lock(g_snapshotMutex);
    return g_snapshot;
}

std::int32_t GetLocalEntityId()
{
    const auto snapshot = GetSnapshot();
    return snapshot ? snapshot->localEntityId : -1;
}

Vector3 GetLocalPosition()
{
    const auto snapshot = GetSnapshot();
    return snapshot ? snapshot->localPosition : Vector3();
}

float GetLocalYaw()
{
    const auto snapshot = GetSnapshot();
    return snapshot ? snapshot->localYaw : 0.f;
}

std::uintptr_t GetGamePtr()
{
    const auto snapshot = GetSnapshot();
    return snapshot ? snapshot->game : 0;
}

std::uintptr_t GetBattleModelPtr()
{
    const auto snapshot = GetSnapshot();
    return snapshot ? snapshot->battleModel : 0;
}

std::uintptr_t GetLocalCharacterPtr()
{
    const auto snapshot = GetSnapshot();
    return snapshot ? snapshot->localCharacter : 0;
}

std::uintptr_t GetCameraModelPtr()
{
    const auto snapshot = GetSnapshot();
    return snapshot ? snapshot->cameraModel : 0;
}

std::uint32_t GetFailureCount()
{
    const auto snapshot = GetSnapshot();
    return snapshot ? snapshot->failureCount : 0;
}

std::string GetLocationId()
{
    std::lock_guard<std::mutex> lk(locationMutex);
    return cachedLocationId;
}

void SetReadCatalogEnabled(bool enabled) { readCatalog.store(enabled); }

struct CameraStepResult {
    bool active = false;
    bool usedPhysical = false;
    bool changed = false;
};

static bool MatrixChanged(const Matrix4x4& a, const Matrix4x4& b)
{
    return std::memcmp(&a.matrix[0][0], &b.matrix[0][0], sizeof(a.matrix)) != 0;
}

static CameraStepResult RefreshCameraStep()
{
    CameraStepResult res{};
    if (g_cameraInvalidated.exchange(false, std::memory_order_acq_rel)) {
        CameraFinder::InvalidateCache();
        PublishNativeCamera(NativeCameraSnapshot{});
    }

    // Fast path: when the native camera is already locked on, skip the managed
    // read entirely and fetch view+projection via cached PA + direct LcRead
    // (~150-300 us on a healthy DMA link).
    if (ConfigInstance.Player.NativeEspCamera) {
        const NativeCameraSnapshot prev = LoadNativeCamera();
        if (prev.valid) {
            CameraFinder::CameraData fast = CameraFinder::GetCameraMatricesFast();
            if (fast.valid) {
                NativeCameraSnapshot snap{};
                snap.view = fast.view;
                snap.projection = fast.projection;
                snap.valid = true;
                snap.positionError = prev.positionError;
                snap.forwardDot = prev.forwardDot;
                snap.poseValid = !fast.position.IsZero();
                if (snap.poseValid) {
                    snap.position = fast.position;
                    snap.forward = fast.forward;
                }
                snap.stampUs = SteadyMicros();
                PublishNativeCamera(snap);
                res.active = true;
                res.usedPhysical = fast.usedPhysical;
                res.changed = MatrixChanged(prev.view, fast.view);
                return res;
            }
        }
    }

    // Full path: read managed camera, then validate/re-resolve native against it.
    RobRunCamera::CameraState managed{};
    bool managedChanged = false;
    const std::uintptr_t model = g_renderCameraModel.load(std::memory_order_acquire);
    if (model) {
        managed = RobRunCamera::Read(model);
        if (managed.valid) {
            static std::uint64_t lastNativeFovMs = 0;
            static float cachedNativeFov = 0.f;
            const std::uint64_t fovNowMs = GetTickCount64();
            if (fovNowMs - lastNativeFovMs >= 8) {
                lastNativeFovMs = fovNowMs;
                const float nf = CameraFinder::ReadNativeFov();
                if (nf > 0.f) cachedNativeFov = nf;
            }
            if (cachedNativeFov > 0.f)
                managed.verticalFov = cachedNativeFov;
            std::lock_guard<std::mutex> lock(g_freshCameraMutex);
            managedChanged = !g_freshCamera.valid ||
                g_freshCamera.position.x != managed.position.x ||
                g_freshCamera.position.y != managed.position.y ||
                g_freshCamera.position.z != managed.position.z ||
                g_freshCamera.forward.x != managed.forward.x ||
                g_freshCamera.forward.y != managed.forward.y ||
                g_freshCamera.forward.z != managed.forward.z;
            g_freshCamera = managed;
        }
    }
    if (!managed.valid) {
        std::lock_guard<std::mutex> lock(g_freshCameraMutex);
        managed = g_freshCamera;
    }
    if (!managed.valid) {
        const auto snapshot = GetSnapshot();
        if (snapshot) managed = snapshot->camera;
    }

    CameraFinder::CameraData native{};
    if (ConfigInstance.Player.NativeEspCamera)
        native = CameraFinder::GetCameraMatricesValidated(
            managed.position, managed.forward, managed.verticalFov);

    const NativeCameraSnapshot prev = LoadNativeCamera();
    NativeCameraSnapshot snap{};
    snap.valid = native.valid;
    if (native.valid) {
        snap.view = native.view;
        snap.projection = native.projection;
    }
    snap.positionError = native.positionError;
    snap.forwardDot = native.forwardDot;
    snap.poseValid = native.valid && !native.position.IsZero();
    if (snap.poseValid) {
        snap.position = native.position;
        snap.forward = native.forward;
    }
    snap.stampUs = SteadyMicros();
    PublishNativeCamera(snap);
    res.active = native.valid || managed.valid;
    res.usedPhysical = native.valid ? native.usedPhysical : managed.usedPhysical;
    res.changed = native.valid
        ? (!prev.valid || MatrixChanged(prev.view, native.view))
        : managedChanged;
    return res;
}

static void WaitCameraInterval(HANDLE timer, std::int64_t micros)
{
    if (timer) {
        LARGE_INTEGER due{};
        due.QuadPart = -micros * 10LL; // 100 ns units, relative
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    std::this_thread::sleep_for(std::chrono::microseconds(micros));
}

static void StartCameraWorker()
{
    StopCameraWorker();
    g_cameraStop.store(false, std::memory_order_relaxed);
    g_cameraRunning.store(true, std::memory_order_release);
    g_cameraThread = std::thread([] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        HANDLE timer = CreateWaitableTimerExW(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

        while (!g_cameraStop.load(std::memory_order_relaxed)) {
            const std::uint64_t tStart = SteadyMicros();
            CameraStepResult res{};
            try {
                const std::uintptr_t fullMapPtr = g_liveFullMapVarPtr.load(std::memory_order_acquire);
                if (fullMapPtr > 0x10000) {
                    struct LiveMapCam {
                        float posX, posY, rot, camSize;
                    } mc{};
                    if (TargetProcess.ReadFresh(fullMapPtr + rr::MapConfigVariance::Position, &mc, sizeof(mc)) &&
                        std::isfinite(mc.posX) && std::isfinite(mc.posY) &&
                        std::isfinite(mc.rot) && std::isfinite(mc.camSize) &&
                        mc.camSize > 0.1f && mc.camSize < 50000.f)
                    {
                        std::lock_guard<std::mutex> lk(g_liveFullMapMutex);
                        g_liveFullMap.pos = Vector2(mc.posX, mc.posY);
                        g_liveFullMap.rot = mc.rot;
                        g_liveFullMap.camSize = mc.camSize;
                        g_liveFullMap.valid = true;
                    }
                }
                res = RefreshCameraStep();
            } catch (const std::exception& e) {
                DLOG("CameraWorker: %s", e.what());
            } catch (...) {
                DLOG("CameraWorker: unknown exception");
            }
            const std::uint64_t tEnd = SteadyMicros();
            const std::uint64_t rttUs = (tEnd >= tStart) ? (tEnd - tStart) : 0;

            if (res.active) {
                g_perfCamReads.fetch_add(1, std::memory_order_relaxed);
                if (res.usedPhysical) g_perfCamPhys.fetch_add(1, std::memory_order_relaxed);
                if (res.changed) g_perfCamChanges.fetch_add(1, std::memory_order_relaxed);
                g_perfCamRttUs.fetch_add(rttUs, std::memory_order_relaxed);
                std::uint64_t prevMax = g_perfCamRttMaxUs.load(std::memory_order_relaxed);
                while (rttUs > prevMax &&
                       !g_perfCamRttMaxUs.compare_exchange_weak(prevMax, rttUs, std::memory_order_relaxed)) {}

                // Pace camera polling to ~1000 Hz using the high-resolution waitable
                // timer (no busy-spin CPU burn, leaves ample FPGA bus headroom for
                // entity/pose scatter reads and input threads).
                constexpr std::uint64_t kTargetCycleUs = 1000;
                const std::uint64_t elapsed = (tEnd >= tStart) ? (tEnd - tStart) : 0;
                if (elapsed < kTargetCycleUs) {
                    WaitCameraInterval(timer, static_cast<std::int64_t>(kTargetCycleUs - elapsed));
                } else {
                    SwitchToThread();
                }
            } else {
                // Back off to 5 ms in menus/lobbies when no camera is active.
                WaitCameraInterval(timer, 5000);
            }
        }
        if (timer) CloseHandle(timer);
        g_cameraRunning.store(false, std::memory_order_release);
    });
}

static void StopCameraWorker()
{
    g_cameraStop.store(true, std::memory_order_relaxed);
    if (g_cameraThread.joinable())
        g_cameraThread.join();
    g_cameraRunning.store(false, std::memory_order_release);
}

void RefreshCamera()
{
    // When the dedicated camera thread is active, callers never block on DMA
    // here — they read the newest seqlock-published sample directly.
    if (g_cameraRunning.load(std::memory_order_acquire))
        return;

    static std::atomic<std::uint64_t> lastRefreshUs{ 0 };
    const std::uint64_t nowUs = SteadyMicros();
    std::uint64_t previous = lastRefreshUs.load(std::memory_order_relaxed);
    if (nowUs - previous < 1000) return;
    if (!lastRefreshUs.compare_exchange_strong(previous, nowUs, std::memory_order_relaxed))
        return;

    RefreshCameraStep();
}

bool GetFreshCamera(RobRunCamera::CameraState& out)
{
    RobRunCamera::CameraState managed{};
    {
        std::lock_guard<std::mutex> lock(g_freshCameraMutex);
        managed = g_freshCamera;
    }

    // The native camera is the one the frame was rendered with. When its pose
    // is known it wins; the managed camera stays as the fallback (and supplies
    // the FOV, which the native fast path does not re-read).
    const NativeCameraSnapshot native = LoadNativeCamera();

    out = managed;
    if (native.poseValid) {
        out.position = native.position;
        out.forward = native.forward;
        out.valid = true;
    }
    return out.valid;
}

bool GetViewProjection(Matrix4x4& out, int screenWidth, int screenHeight)
{
    if (screenWidth <= 0 || screenHeight <= 0) return false;

    const NativeCameraSnapshot native = LoadNativeCamera();
    if (native.valid) {
        // Use the game camera's actual projection matrix untouched so NDC [-1, +1]
        // maps linearly to the full overlay surface on BOTH native aspect ratios
        // AND stretched resolutions (e.g. 2560x1440 stretched onto a 3440x1440 display).
        Matrix4x4 combined = native.view * native.projection;
        bool valid = true;
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                if (!std::isfinite(combined.matrix[r][c])) {
                    valid = false;
                    break;
                }
            }
            if (!valid) break;
        }
        if (valid && (std::abs(combined.matrix[2][3]) > 0.001f || std::abs(combined.matrix[3][2]) > 0.001f)) {
            out = combined;
            return true;
        }
    }

    const auto snapshot = GetSnapshot();
    RobRunCamera::CameraState camera;
    {
        std::lock_guard<std::mutex> lock(g_freshCameraMutex);
        camera = g_freshCamera;
    }
    if (!camera.valid) {
        if (!snapshot) return false;
        camera = snapshot->camera;
    }
    if (!camera.valid) return false;

    // Prefer the game's actual UI/render aspect ratio (e.g. 2560x1440 -> 1.778)
    // over the overlay display aspect (e.g. 3440x1440 -> 2.389) so stretched
    // resolutions stay horizontally aligned when using the managed camera path.
    float aspect = static_cast<float>(screenWidth) / static_cast<float>(screenHeight);
    if (snapshot && snapshot->inGameMap.valid) {
        const auto& mv = snapshot->inGameMap.minimap;
        if (mv.rectValid && mv.uiScreenW > 100.f && mv.uiScreenH > 100.f) {
            const float gameAspect = mv.uiScreenW / mv.uiScreenH;
            if (std::isfinite(gameAspect) && gameAspect >= 1.0f && gameAspect <= 4.0f)
                aspect = gameAspect;
        }
    }
    return RobRunCamera::BuildViewProjection(camera, aspect, out);
}

bool GetFallbackViewProjection(Matrix4x4& out)
{
    const auto snapshot = GetSnapshot();
    if (!snapshot || !snapshot->fallbackViewProjectionValid) return false;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (!std::isfinite(snapshot->fallbackViewProjection.matrix[r][c])) return false;
        }
    }
    out = snapshot->fallbackViewProjection;
    return true;
}

static thread_local bool tl_frameActive = false;
static thread_local bool tl_frameValid = false;
static thread_local int tl_frameW = 0;
static thread_local int tl_frameH = 0;
static thread_local Matrix4x4 tl_frameVP{};

void BeginRenderFrame(int screenWidth, int screenHeight)
{
    tl_frameActive = true;
    tl_frameW = screenWidth;
    tl_frameH = screenHeight;
    tl_frameValid = false;
    if (screenWidth <= 0 || screenHeight <= 0)
        return;
    if (GetViewProjection(tl_frameVP, screenWidth, screenHeight) ||
        GetFallbackViewProjection(tl_frameVP)) {
        tl_frameValid = true;
    }
}

void EndRenderFrame()
{
    tl_frameActive = false;
    tl_frameValid = false;
}

bool ResolveViewProjection(Matrix4x4& out, int screenWidth, int screenHeight)
{
    if (screenWidth <= 0 || screenHeight <= 0) return false;
    if (tl_frameActive && tl_frameW == screenWidth && tl_frameH == screenHeight) {
        if (!tl_frameValid) return false;
        out = tl_frameVP;
        return true;
    }
    if (GetViewProjection(out, screenWidth, screenHeight)) return true;
    return GetFallbackViewProjection(out);
}

std::string RoleLabel(int role, const std::string& name)
{
    if (!name.empty()) return name;
    if (role == 1257638368) return "Criminal";
    if (role == 6) return "Hubber";
    if (role > 0 && role < 100000) return "Role:" + std::to_string(role);
    return {};
}

const char* GetViewProjectionSource()
{
    if (LoadNativeCamera().valid) return "native";
    {
        std::lock_guard<std::mutex> lock(g_freshCameraMutex);
        if (g_freshCamera.valid) return "managed";
    }
    const auto snapshot = GetSnapshot();
    if (snapshot && snapshot->camera.valid) return "snapshot";
    return "none";
}

CameraDebug GetCameraDebug()
{
    CameraDebug debug;
    debug.nativeEnabled = ConfigInstance.Player.NativeEspCamera;
    const NativeCameraSnapshot native = LoadNativeCamera();
    debug.nativeValid = native.valid;
    debug.positionError = native.positionError;
    debug.forwardDot = native.forwardDot;
    const auto snapshot = GetSnapshot();
    if (snapshot && snapshot->stampMs != 0)
        debug.snapshotAgeMs =
            static_cast<std::int64_t>(GetTickCount64() - snapshot->stampMs);
    return debug;
}

Vector2 WorldToScreen(const Matrix4x4& viewProjection, const Vector3& world,
    int screenWidth, int screenHeight)
{
    Vector2 out(0.f, 0.f);
    if (!RobRunCamera::WorldToScreen(viewProjection, world, screenWidth, screenHeight, out))
        return Vector2(0.f, 0.f);
    return out;
}

bool ProjectWorld(const Matrix4x4& viewProjection, const Vector3& world,
    int screenWidth, int screenHeight, Vector2& out)
{
    if (screenWidth <= 0 || screenHeight <= 0) return false;
    if (!RobRunMemory::Finite(world)) return false;
    return RobRunCamera::WorldToScreen(viewProjection, world, screenWidth, screenHeight, out);
}

BvhStatus GetBvhStatus()
{
    const auto bvh = LoadBvhSnapshot();
    BvhStatus s;
    if (bvh) {
        s.loaded = bvh->valid;
        s.nodeCount = (uint32_t)bvh->nodes.size();
        s.triCount = (uint32_t)bvh->tris.size();
        s.path = bvh->loadedPath;
    }
    return s;
}

void ReloadBvh()
{
    TryLoadBvh(true);
}

std::shared_ptr<const CollisionBVH::Scene> GetBvhScene()
{
    return LoadBvhSnapshot();
}

bool IsOccluded(const Vector3& origin, const Vector3& target, float tMin, bool checkDynamic)
{
    const auto bvh = LoadBvhSnapshot();
    if (!bvh || !bvh->valid) return false;
    const CollisionBVH::Float3 o{ origin.x, origin.y, origin.z };
    const CollisionBVH::Float3 t{ target.x, target.y, target.z };
    CollisionBVH::DynamicOccluders dynCopy;
    const CollisionBVH::DynamicOccluders* dyn = nullptr;
    if (checkDynamic) {
        std::lock_guard<std::mutex> lock(g_dynOccludersMutex);
        dynCopy = g_dynOccluders;
        dyn = &dynCopy;
    }
    return CollisionBVH::IsOccluded(*bvh, o, t, dyn, ConfigInstance.Player.CarCabinSolid, tMin);
}

bool IsCameraOccluded(const Vector3& target, float tMin, bool checkDynamic)
{
    if (!IsBvhActiveForCurrentLocation()) return false;
    const auto bvh = LoadBvhSnapshot();
    if (!bvh || !bvh->valid) return false;
    RobRunCamera::CameraState cam{};
    if (!GetFreshCamera(cam)) {
        const auto snap = GetSnapshot();
        if (snap) cam = snap->camera;
    }
    if (!cam.valid) return false;
    const CollisionBVH::Float3 o{ cam.position.x, cam.position.y, cam.position.z };
    const CollisionBVH::Float3 t{ target.x, target.y, target.z };
    CollisionBVH::DynamicOccluders dynCopy;
    const CollisionBVH::DynamicOccluders* dyn = nullptr;
    if (checkDynamic) {
        std::lock_guard<std::mutex> lock(g_dynOccludersMutex);
        dynCopy = g_dynOccluders;
        dyn = &dynCopy;
    }
    return CollisionBVH::IsOccluded(*bvh, o, t, dyn, ConfigInstance.Player.CarCabinSolid, tMin);
}

bool GetFreshFullMapVariance(Vector2& pos, float& rot, float& cameraSize)
{
    std::lock_guard<std::mutex> lk(g_liveFullMapMutex);
    if (!g_liveFullMap.valid)
        return false;
    pos = g_liveFullMap.pos;
    rot = g_liveFullMap.rot;
    cameraSize = g_liveFullMap.camSize;
    return true;
}

} // namespace RobRunReader
