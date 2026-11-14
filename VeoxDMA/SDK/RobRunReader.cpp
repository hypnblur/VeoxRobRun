#include "pch.h"
#include "RobRunReader.h"
#include "RobRunMemory.h"
#include "Il2Cpp.h"
#include "Memory.h"
#include "CameraFinder.h"
#include "CollisionBVH.h"
#include "ConfigInstance.h"
#include <atomic>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace RobRunReader {

static std::uintptr_t gaBase = 0;
static std::uintptr_t upBase = 0;

static std::uintptr_t cachedGame = 0;
static std::uintptr_t cachedBattleModel = 0;
static std::uintptr_t cachedBattleEntities = 0;
static std::uintptr_t cachedCameraModel = 0;
static std::uintptr_t cachedLocalCharacter = 0;
static std::uintptr_t cachedLookDirectionModel = 0;
static std::uintptr_t cachedLookDirDirect = 0;
static std::uintptr_t cachedPlayerRotationInput = 0;
static std::uintptr_t cachedUpdater = 0;

// Scene/session identity. Every root change (level transition, new match)
// bumps the generation so memory aim can stop writing until the new objects
// have been re-resolved.
static int32_t localEntityId = -1;
static std::uint64_t sessionGeneration = 0;
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

static void MarkSessionChanged()
{
    ++sessionGeneration;
    sessionChangedMs = GetTickCount64();
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
static std::uint64_t worldRefreshStamp = 0;
static std::uint64_t inventoryRefreshStamp = 0;
static RobRunCamera::CameraState cameraState;
static Vector3 localPosition{};
static std::uint32_t failureCount = 0;
static std::uint32_t poseFailureCount = 0;
static std::atomic<bool> readCatalog{ false };
static int updateCounter = 0;
static int gameRetryCountdown = 0;
static int updaterRetryCountdown = 0;
static int battleEntitiesRetryCountdown = 0;

static Matrix4x4 fallbackViewProjection{};
static bool fallbackViewProjectionValid = false;

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
    int weaponIndex = -1;
    bool isFiring = false;
    bool isReloading = false;
    bool hasArmor = false;
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
};
static std::unordered_map<std::int32_t, RigCacheEntry> rigCache;

static void PruneCaches()
{
    // Entity IDs are stable within a match but the caches should not grow
    // without bound across map changes. Clearing is cheap: layouts rebuild on
    // the next pass.
    if (rigCache.size() > 1024) rigCache.clear();
    if (detailCache.size() > 1024) detailCache.clear();
}

// Worker-published snapshot. Only the worker writes; any thread may read.
static std::mutex g_snapshotMutex;
static std::shared_ptr<const Snapshot> g_snapshot = std::make_shared<Snapshot>();
static std::atomic<bool> g_workerStop{ false };
static std::thread g_workerThread;
static std::atomic<bool> g_worldWorkerStop{ false };
static std::thread g_worldWorkerThread;

// Render-thread camera refresh. The worker publishes cachedCameraModel via
// this atomic; RefreshCamera() re-reads the camera block with uncached DMA
// and stores it here so GetViewProjection() can use it instead of the
// worker's ~30 Hz snapshot.
static std::atomic<std::uintptr_t> g_renderCameraModel{ 0 };
static std::mutex g_freshCameraMutex;
static RobRunCamera::CameraState g_freshCamera;

// Native Unity main-camera view-projection, re-read on the render thread.
// Memory aim rewrites the managed BattleCameraModel block, so building the ESP
// matrix from that block makes the overlay project with a camera the game never
// rendered (skeletons drift/lag behind the character). The native Camera
// component is the camera the GPU actually used, so it is preferred whenever it
// is readable; the managed block stays as the fallback.
static std::mutex g_nativeCameraMutex;
static Matrix4x4 g_nativeView{};
static Matrix4x4 g_nativeProjection{};
static bool g_nativeViewProjectionValid = false;
static float g_nativePositionError = -1.f;
static float g_nativeForwardDot = -1.f;

static CollisionBVH::Scene g_bvhScene;
static std::string g_bvhLoadedPath;

static void TryLoadBvh(bool force = false)
{
    const std::string& path = ConfigInstance.Player.BvhPath.empty()
        ? "city_colliders.bvh"
        : ConfigInstance.Player.BvhPath;

    static std::uint64_t s_lastAttemptMs = 0;
    static std::string s_lastAttemptPath;
    const std::uint64_t now = GetTickCount64();

    if (!force)
    {
        if (g_bvhScene.valid && path == s_lastAttemptPath)
            return;
        if (s_lastAttemptMs != 0 && (now - s_lastAttemptMs) < 3000 && path == s_lastAttemptPath)
            return;
    }

    s_lastAttemptMs = now ? now : 1;
    s_lastAttemptPath = path;

    g_bvhScene = CollisionBVH::Scene{};
    if (CollisionBVH::Load(path, g_bvhScene))
    {
        g_bvhLoadedPath = g_bvhScene.loadedPath;
    }
    else
    {
        g_bvhLoadedPath.clear();
    }
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
    for (const auto& entry : cachedViews)
    {
        ViewCandidate candidate;
        candidate.view = entry.second;
        Skeleton::Vec root{};
        if (entry.second > 0x10000 &&
            Skeleton::RootWorldPosition(entry.second, DmaRead, root))
        {
            candidate.position = Vector3{ root.x, root.y, root.z };
            candidate.hasPosition = RobRunMemory::Finite(candidate.position);
        }
        views.push_back(candidate);
    }

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
        if (candidate.hasPosition)
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
            if (!views[v].hasPosition || views[v].claimed) continue;
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
    snapshot->entities = entities;
    {
        std::lock_guard<std::mutex> lock(g_worldMutex);
        snapshot->world = g_worldState;
        snapshot->inventory = g_inventoryState;
    }
    snapshot->camera = cameraState;
    snapshot->localEntityId = localEntityId;
    snapshot->localPosition = localPosition;
    snapshot->game = cachedGame;
    snapshot->battleModel = cachedBattleModel;
    snapshot->localCharacter = cachedLocalCharacter;
    snapshot->cameraModel = cachedCameraModel;
    snapshot->lookDirectionModel = cachedLookDirectionModel;
    snapshot->lookDirDirect = cachedLookDirDirect;
    snapshot->playerRotationInput = cachedPlayerRotationInput;
    snapshot->failureCount = failureCount;
    snapshot->poseFailures = poseFailureCount;
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
    g_workerStop.store(true, std::memory_order_relaxed);
    g_worldWorkerStop.store(true, std::memory_order_relaxed);
    if (g_workerThread.joinable())
        g_workerThread.join();
    if (g_worldWorkerThread.joinable())
        g_worldWorkerThread.join();
}

void Init(std::uintptr_t gameAssemblyBase, std::uintptr_t unityPlayerBase)
{
    Shutdown();

    gaBase = gameAssemblyBase;
    upBase = unityPlayerBase;
    Il2Cpp::Init(gameAssemblyBase);
    RobRunCamera::Init(gameAssemblyBase);
    RobRunInventory::Init(gameAssemblyBase);
    RobRunWorld::Init(gameAssemblyBase);
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
        g_inventoryState = std::make_shared<RobRunInventory::State>();
    }
    worldRefreshStamp = 0;
    inventoryRefreshStamp = 0;
    cameraState = RobRunCamera::CameraState{};
    localEntityId = -1;
    localPosition = Vector3();
    fallbackViewProjection = Matrix4x4{};
    fallbackViewProjectionValid = false;

    g_renderCameraModel.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_freshCameraMutex);
        g_freshCamera = RobRunCamera::CameraState{};
    }

    TryLoadBvh(true);

    {
        std::lock_guard<std::mutex> lock(g_snapshotMutex);
        g_snapshot = std::make_shared<Snapshot>();
    }

    g_workerStop.store(false, std::memory_order_relaxed);
    g_workerThread = std::thread([] {
        while (!g_workerStop.load(std::memory_order_relaxed)) {
            Update();
            // Small fixed pause so a healthy pass runs at ~30 Hz while a slow
            // one simply runs slower instead of blocking the render thread.
            for (int i = 0; i < 10 && !g_workerStop.load(std::memory_order_relaxed); ++i)
                Sleep(1);
        }
    });

    g_worldWorkerStop.store(false, std::memory_order_relaxed);
    g_worldWorkerThread = std::thread([] {
        while (!g_worldWorkerStop.load(std::memory_order_relaxed)) {
            const std::uintptr_t bm = cachedBattleModel;
            const std::uintptr_t be = cachedBattleEntities;
            const std::uintptr_t lc = cachedLocalCharacter;
            if (bm > 0x10000) {
                auto state = std::make_shared<RobRunWorld::State>();
                if (RobRunWorld::Read(bm, be, *state)) {
                    std::lock_guard<std::mutex> lock(g_worldMutex);
                    g_worldState = std::move(state);
                }
            }
            if (lc > 0x10000) {
                auto inv = std::make_shared<RobRunInventory::State>();
                if (RobRunInventory::Read(bm, lc, *inv, readCatalog.load())) {
                    std::lock_guard<std::mutex> lock(g_worldMutex);
                    g_inventoryState = std::move(inv);
                }
            }
            for (int i = 0; i < 25 && !g_worldWorkerStop.load(std::memory_order_relaxed); ++i)
                Sleep(10);
        }
    });

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

    const std::uintptr_t healthWrapper = ReadPtr(healthModel + rr::HealthModel::Health);
    float health = 0.f;
    if (Il2Cpp::ReadPrimitiveFloat(healthWrapper, health) &&
        std::isfinite(health) && health >= 0.f && health <= 100000.f) {
        entity.health = health;
        entity.healthKnown = true;
    }

    const std::int32_t maxAmount = RobRunMemory::ReadInt32(healthModel + rr::HealthModel::MaxAmount);
    if (maxAmount > 0 && maxAmount <= 100000)
        entity.maxHealth = static_cast<float>(maxAmount);

    const std::int32_t critical = RobRunMemory::ReadInt32(
        healthModel + rr::HealthModel::CriticalHealth);
    if (critical >= 0 && critical <= 100000)
        entity.criticalHealth = static_cast<float>(critical);

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

    if (lower.find("crim") != std::string::npos ||
        lower.find("rob") != std::string::npos ||
        lower.find("thief") != std::string::npos ||
        lower.find("bandit") != std::string::npos ||
        lower.find("gang") != std::string::npos)
        return "Criminal";
    if (lower.find("polic") != std::string::npos ||
        lower.find("cop") != std::string::npos ||
        lower.find("officer") != std::string::npos ||
        lower.find("swat") != std::string::npos ||
        lower.find("guard") != std::string::npos ||
        lower.find("sheriff") != std::string::npos)
        return "Police Officer";
    if (lower.find("civil") != std::string::npos ||
        lower.find("citizen") != std::string::npos ||
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

    int roleId = 0;
    std::string roleName;
    if (ResolveRoleFromObject(roleWrapper, roleId, roleName)) {
        if (!roleName.empty()) entity.roleName = std::move(roleName);
        if (roleId != 0) entity.role = roleId;
        if (!entity.roleName.empty()) return;
    }

    const std::uintptr_t innerPtr = ReadPtr(roleWrapper + rr::ReferenceData::Value);
    if (innerPtr > 0x10000 && Il2Cpp::ClassOf(innerPtr) != 0)
        return;

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

    ApplyRoleFromWrapper(ReadPtr(common + rr::CommonModel::Role), entity);

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

    // 2) KnockdownModel._endTs (active while after the sentinel and now).
    const std::uintptr_t kdModel = ReadPtr(
        characterModel + rr::BattleCharacterModel::Knockdown);
    if (kdModel > 0x10000 && battleNowMs > 0) {
        const std::int64_t kdEndTs = ReadTimestampData(
            ReadPtr(kdModel + rr::KnockdownModel::EndTs));
        if (kdEndTs > kSentinelTs && kdEndTs > battleNowMs)
            entity.isKnockedDown = true;
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
    const std::uintptr_t nameStr = ReadPtr(
        description + rr::Description::Name);
    if (nameStr > 0x10000)
        RobRunMemory::ReadString(nameStr, out);
    const std::int32_t idx = RobRunMemory::ReadInt32(
        description + rr::InventoryItemDescription::Index);
    if (idx >= 0) weaponIndex = idx;
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

    if (RobRunMemory::IsClass(value, "String") &&
        RobRunMemory::ReadString(value, out) && !out.empty())
        return true;

    const std::uintptr_t wrapped = ReadPtr(value + rr::BattleCharacterSlotData::wrapper_value);
    if (wrapped > 0x10000 && wrapped != value &&
        RobRunMemory::IsClass(wrapped, "String") &&
        RobRunMemory::ReadString(wrapped, out) && !out.empty())
        return true;

    out.clear();
    return false;
}

static void ReadSlotIdentity(std::uintptr_t slotValue, EntityData& entity)
{
    if (slotValue <= 0x10000) return;

    // New offset set: the dictionary value is BattleCharacterSlotData itself.
    ReadSlotString(slotValue, rr::BattleCharacterSlotData::Nickname, entity.nickname);
    ReadSlotString(slotValue, rr::BattleCharacterSlotData::ClanTag, entity.clanTag);

    // The party filter compares public ids, so read it from the same object.
    const std::int64_t publicId = RobRunMemory::ReadInt64(
        slotValue + rr::BattleCharacterSlotData::PublicId);
    if (publicId > 0) entity.publicId = static_cast<std::uint64_t>(publicId);

    // Legacy: the dictionary value is BattleCharacterSlotModel, which points at
    // its data object at +0x10.
    if (entity.nickname.empty() || entity.clanTag.empty() || entity.publicId == 0) {
        const std::uintptr_t slotData = ReadPtr(
            slotValue + rr::BattleCharacterSlotModel::_data);
        if (slotData > 0x10000 && slotData != slotValue) {
            if (entity.nickname.empty())
                ReadSlotString(slotData, rr::BattleCharacterSlotData::Nickname, entity.nickname);
            if (entity.clanTag.empty())
                ReadSlotString(slotData, rr::BattleCharacterSlotData::ClanTag, entity.clanTag);
            if (entity.publicId == 0) {
                const std::int64_t legacyId = RobRunMemory::ReadInt64(
                    slotData + rr::BattleCharacterSlotData::PublicId);
                if (legacyId > 0) entity.publicId = static_cast<std::uint64_t>(legacyId);
            }
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

    bool localPresent = false;
    if (localPublicId > 0) {
        localPresent = std::find(ids.publicIds.begin(), ids.publicIds.end(),
            localPublicId) != ids.publicIds.end();
    }
    if (!localPresent && localId > 0) {
        localPresent = std::find(ids.entityIds.begin(), ids.entityIds.end(),
            localId) != ids.entityIds.end();
    }

    if (!any || !localPresent)
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
        fingerprintFailures = 0;
        return;
    }
    fingerprintFailures = 0;
    (void)Il2Cpp::VerifyAccessors();

    if (cachedGame && !IsClass(cachedGame, "Game"))
        cachedGame = 0;
    if (cachedBattleModel && !IsClass(cachedBattleModel, "BattleModel"))
        cachedBattleModel = 0;
    if (cachedBattleEntities && !IsClass(cachedBattleEntities, "BattleEntitiesModel"))
        cachedBattleEntities = 0;
    if (cachedUpdater && !IsClass(cachedUpdater, "BattleEntitiesUpdater"))
        cachedUpdater = 0;
}

static void UpdateInternal()
{
    entities.clear();
    localEntityId = -1;
    localPosition = Vector3{};
    fallbackViewProjection = Matrix4x4{};
    fallbackViewProjectionValid = false;
    poseFailureCount = 0;

    if (!gaBase) return;

    ++updateCounter;
    if ((updateCounter % 120) == 0)
        RevalidateSession();

    if (!cachedGame) {
        if (gameRetryCountdown <= 0) {
            cachedGame = ResolveGameViaGom();
            // The GOM walk is the most expensive resolution step. When it
            // fails (menu/loading screen) back off instead of walking 8k nodes
            // on every pass.
            gameRetryCountdown = cachedGame ? 0 : 30;
        } else {
            --gameRetryCountdown;
        }
    }

    if (!cachedBattleModel)
        cachedBattleModel = ResolveBattleModel(cachedGame);

    if (!cachedBattleModel) {
        {
            std::lock_guard<std::mutex> lock(g_worldMutex);
            g_worldState = std::make_shared<RobRunWorld::State>();
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
        NoteSessionState();
        ++failureCount;
        return;
    }

    if (!cachedBattleEntities) {
        if (battleEntitiesRetryCountdown <= 0) {
            const std::uintptr_t candidate = ReadPtr(
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
        cachedCameraModel = 0;
        cachedLookDirectionModel = 0;
        cachedLookDirDirect = 0;
        cachedPlayerRotationInput = 0;
        cachedLocalCharacter = 0;
        NoteSessionState();
        ++failureCount;
        return;
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
    cameraState = RobRunCamera::Read(cachedCameraModel);

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
            Vector3 position{};
            const std::uintptr_t positionAddress =
                transform + rr::CharacterTransformModel::Position;
            if ((RobRunMemory::ReadBytesFresh(positionAddress, &position, sizeof(position)) ||
                 RobRunMemory::ReadBytes(positionAddress, &position, sizeof(position))) &&
                RobRunMemory::Finite(position)) {
                localPosition = position;
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
            cachedViews.clear();
            const std::uintptr_t viewDictionary = ReadPtr(
                cachedUpdater + rr::BattleEntitiesUpdater::_characterEntityViews);
            std::vector<RobRunMemory::DictEntry> viewEntries;
            if (RobRunMemory::ReadDictInt(viewDictionary, viewEntries)) {
                cachedViews.reserve(viewEntries.size());
                for (const auto& entry : viewEntries)
                    cachedViews[entry.key] = entry.value;
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
        if (slotRefreshStamp == 0 || slotNow - slotRefreshStamp >= 5000)
        {
            slotRefreshStamp = slotNow;
            cachedSlots.clear();

            std::vector<RobRunMemory::DictEntry> slotEntries;
            bool slotsRead = false;

            const std::uintptr_t slotsModel = ReadPtr(
                cachedBattleModel + rr::BattleModel::CharacterSlotsModel);
            if (slotsModel > 0x10000) {
                std::uintptr_t slotDictionary = ReadPtr(
                    slotsModel + rr::CharacterSlotsModel::SlotDict);
                slotsRead = RobRunMemory::ReadDictInt(slotDictionary, slotEntries);
                if (!slotsRead) {
                    slotDictionary = ReadPtr(
                        slotsModel + rr::CharacterSlotsModel::SlotDictLegacy);
                    slotsRead = RobRunMemory::ReadDictInt(slotDictionary, slotEntries);
                }
            }

            if (!slotsRead) {
                const std::uintptr_t legacyModel = ReadPtr(
                    cachedBattleModel + rr::BattleModel::CharacterSlotsModelLegacy);
                if (legacyModel > 0x10000) {
                    std::uintptr_t slotDictionary = ReadPtr(
                        legacyModel + rr::CharacterSlotsModel::SlotDictLegacy);
                    slotsRead = RobRunMemory::ReadDictInt(slotDictionary, slotEntries);
                }
            }

            if (!slotsRead) {
                const std::uintptr_t slotsRoot = ReadPtr(
                    cachedBattleEntities + rr::BattleEntitiesModel::CharacterSlotsRoot);
                if (slotsRoot > 0x10000) {
                    const std::uintptr_t slotDictionary = ReadPtr(
                        slotsRoot + rr::CharacterSlotsRoot::SlotDict);
                    slotsRead = RobRunMemory::ReadDictInt(slotDictionary, slotEntries);
                }
            }

            if (slotsRead) {
                cachedSlots.reserve(slotEntries.size());
                for (const auto& entry : slotEntries)
                    cachedSlots[entry.key] = entry.value;
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

    const std::uintptr_t entityDictionary = ReadPtr(
        cachedBattleEntities + rr::BattleEntitiesModel::EntityDict);
    std::vector<RobRunMemory::DictEntry> entityEntries;
    if (RobRunMemory::ReadDictInt(entityDictionary, entityEntries)) {
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
            std::uintptr_t simulatedModel = 0;
        };
        std::vector<PreResolve> pre;
        pre.reserve(entityEntries.size());
        std::vector<std::size_t> needResolve;

        const std::uint64_t resolveNow = GetTickCount64();
        const bool forceFullResolve = resolveNow - fullResolveStamp >= 2000;
        if (forceFullResolve) fullResolveStamp = resolveNow;

        for (const auto& entry : entityEntries) {
            if (entry.value <= 0x10000) continue;
            PreResolve p{ entry.key, entry.value, 0, 0 };
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
            auto sc = TargetProcess.CreateScatterHandleCached();
            if (sc) {
                for (auto idx : needResolve) {
                    TargetProcess.AddScatterReadRequest(sc,
                        pre[idx].entityModel + rr::BattleCharacterEntityModel::ClientModel,
                        &pre[idx].clientModel, sizeof(std::uintptr_t));
                    TargetProcess.AddScatterReadRequest(sc,
                        pre[idx].entityModel + rr::BattleCharacterEntityModel::SimulatedModel,
                        &pre[idx].simulatedModel, sizeof(std::uintptr_t));
                }
                TargetProcess.ExecuteScatterReadCached(sc);
                TargetProcess.CloseScatterHandle(sc);
            }
        }

        for (auto idx : needResolve) {
            auto& p = pre[idx];
            std::uintptr_t characterModel = p.clientModel;
            if (characterModel <= 0x10000) characterModel = 0;

            if (characterModel > 0x10000 &&
                !IsClass(characterModel, "BattleCharacterModel"))
            {
                if (p.simulatedModel > 0x10000 &&
                    IsClass(p.simulatedModel, "BattleCharacterModel"))
                    characterModel = p.simulatedModel;
                else if (IsClass(p.entityModel, "BattleCharacterModel"))
                    characterModel = p.entityModel;
                else
                    characterModel = 0;
            }
            p.clientModel = characterModel;
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
            auto sc = TargetProcess.CreateScatterHandleCached();
            if (sc) {
                for (auto idx : needTransform)
                    TargetProcess.AddScatterReadRequest(sc,
                        work[idx].characterModel +
                            rr::BattleCharacterModel::CharacterTransformModel,
                        &work[idx].transform, sizeof(std::uintptr_t));
                TargetProcess.ExecuteScatterReadCached(sc);
                TargetProcess.CloseScatterHandle(sc);
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

        for (auto& w : work) {
            const auto viewIt = views.find(w.entityId);
            const std::uintptr_t viewPtr =
                (viewIt != views.end()) ? viewIt->second : 0;
            auto rigIt = rigCache.find(w.entityId);
            if (rigIt != rigCache.end() && rigIt->second.layoutValid &&
                rigIt->second.view == viewPtr)
            {
                const std::size_t sz = Skeleton::PoseDataSize(rigIt->second.layout);
                w.needsPose = true;
                w.poseOffset = poseTotal;
                w.poseSize = sz;
                poseTotal += sz;
            }
        }
        poseBuf.resize(poseTotal, 0);

        // ---------------------------------------------------------------
        // Phase 2: Scatter-read positions AND poses in one DMA call
        // so skeletons are sampled at the same instant as positions.
        // ---------------------------------------------------------------
        if (!work.empty()) {
            auto scatter = TargetProcess.CreateScatterHandle();
            if (scatter) {
                for (auto& w : work) {
                    TargetProcess.AddScatterReadRequest(scatter,
                        w.blockAddress, &w.block, sizeof(TransformBlock));
                    if (w.needsPose) {
                        auto& rig = rigCache[w.entityId];
                        TargetProcess.AddScatterReadRequest(scatter,
                            rig.layout.matrices,
                            poseBuf.data() + w.poseOffset, w.poseSize);
                    }
                }
                TargetProcess.ExecuteReadScatter(scatter);
                TargetProcess.CloseScatterHandle(scatter);
            }
        }

        for (auto& w : work) {
            if (!RobRunMemory::Finite(w.block.position) ||
                (w.block.position.x == 0.f && w.block.position.y == 0.f &&
                 w.block.position.z == 0.f))
            {
                w.block = TransformBlock{};
                if (!RobRunMemory::ReadBytesFresh(w.blockAddress, &w.block,
                        sizeof(TransformBlock)) &&
                    !RobRunMemory::ReadBytes(w.blockAddress, &w.block,
                        sizeof(TransformBlock)))
                {
                    w.positionValid = false;
                    continue;
                }
            }
            w.positionValid = RobRunMemory::Finite(w.block.position);
        }

        // ---------------------------------------------------------------
        // Phase 3: Rig layout rebuilds and late pose collection.
        // Layouts already valid had their pose read above; newly
        // rebuilt layouts get a supplementary scatter.
        // ---------------------------------------------------------------
        const std::uint64_t now = GetTickCount64();
        std::vector<std::size_t> lateResolve;

        for (std::size_t wi = 0; wi < work.size(); ++wi) {
            auto& w = work[wi];
            if (!w.positionValid) continue;

            const auto viewIt = views.find(w.entityId);
            const std::uintptr_t viewPtr =
                (viewIt != views.end()) ? viewIt->second : 0;

            auto& rig = rigCache[w.entityId];
            if (rig.view != viewPtr) {
                rig.view = viewPtr;
                rig.layoutValid = false;
                rig.layoutStamp = 0;
                rig.bones.clear();
                rig.points = Skeleton::BonePoints{};
                rig.prevLocalValid = false;
                rig.smoothedValid = false;
                rig.lastSampledPos = Vector3{};
                rig.lastMotionTick = 0;
                rig.animatorActive = true;
            }

            // Cheap liveness check every 1.5 s: three pointer reads instead of
            // a full rebuild. Only a failed check forces the expensive path,
            // so a healthy rig keeps the exact same bone indices (and the
            // smoothed pose that goes with them) for as long as it exists.
            if (rig.layoutValid && now - rig.layoutStamp >= 1500) {
                rig.layoutStamp = now;
                if (viewPtr <= 0x10000 ||
                    !Skeleton::ValidateHierarchy(rig.layout, DmaRead)) {
                    rig.layoutValid = false;
                    rig.layoutStamp = 0;
                }
            }
            const bool dueForRebuild = !rig.layoutValid && viewPtr > 0x10000 &&
                (rig.layoutStamp == 0 || now - rig.layoutStamp >= 250);
            if (dueForRebuild) {
                rig.layoutStamp = now;
                Skeleton::RigLayout candidate;
                bool ok = Skeleton::BuildLayoutFromView(viewPtr, DmaRead, candidate);
                if (!ok)
                    ok = Skeleton::BuildLayoutByName(viewPtr, DmaRead, candidate);
                if (ok) {
                    rig.layout = std::move(candidate);
                    rig.layoutValid = true;
                    rig.poseFailures = 0;
                    rig.poseFailureSince = 0;
                    // New indices mean the old smoothed segments describe a
                    // different bone set; restart from the fresh sample.
                    rig.smoothedValid = false;
                } else {
                    if (rig.poseFailureSince == 0)
                        rig.poseFailureSince = now;
                    if (now - rig.poseFailureSince >= 2500) {
                        rig.bones.clear();
                        rig.points = Skeleton::BonePoints{};
                        rig.smoothedValid = false;
                    }
                }
            }

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
        {
            std::lock_guard<std::mutex> lock(g_freshCameraMutex);
            if (g_freshCamera.valid) visCamera = g_freshCamera;
        }
        int detailBudget = 3;
        const std::int64_t battleNowMs = ReadBattleNowMs(cachedBattleEntities);
        for (auto& w : work) {
            if (!w.positionValid) continue;

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

            auto& detail = detailCache[entity.entityId];
            const bool detailModelChanged =
                detail.characterModel != w.characterModel;
            const bool detailExpired = now - detail.stamp > 500;
            if (detailModelChanged ||
                (detailExpired && detailBudget > 0))
            {
                if (!detailModelChanged) --detailBudget;
                detail = DetailCacheEntry{};
                detail.characterModel = w.characterModel;
                detail.transform = w.transform;
                detail.stamp = now;

                EntityData scratch{};
                scratch.entityId = entity.entityId;

                const std::uintptr_t deadWrapper = ReadPtr(
                    w.characterModel + rr::BattleCharacterModel::IsDeadWrapper);
                bool dead = false;
                if (Il2Cpp::ReadPrimitiveBool(deadWrapper, dead))
                    detail.isDead = dead;

                ReadHealth(w.characterModel, scratch);
                detail.health = scratch.health;
                detail.maxHealth = scratch.maxHealth;
                detail.criticalHealth = scratch.criticalHealth;
                detail.healthKnown = scratch.healthKnown;

                scratch.isDead = detail.isDead;
                ReadRoleAndVehicle(w.characterModel, scratch);
                ReadCommonState(w.characterModel, scratch);
                ReadReloadAndKnockdownState(w.characterModel, battleNowMs, scratch);
                detail.isReloading = scratch.isReloading;
                detail.isKnockedDown = scratch.isKnockedDown;
                detail.isKnockedOut = scratch.isKnockedOut;
                detail.isSurrendered = scratch.isSurrendered;
                detail.isHandcuffed = scratch.isHandcuffed;
                detail.reloadPhase = std::move(scratch.reloadPhase);
                detail.isDead = scratch.isDead;
                detail.isSprinting = scratch.isSprinting;
                detail.isCrouching = scratch.isCrouching;
                detail.hasArmor = scratch.hasArmor;
                detail.isFiring = scratch.isFiring;
                detail.role = scratch.role;
                detail.roleName = std::move(scratch.roleName);
                detail.inVehicle = scratch.inVehicle;
                detail.vehicleId = scratch.vehicleId;

                ReadWeaponName(w.characterModel, detail.weaponName, detail.weaponIndex);

                const auto slotIt = slots.find(w.entityId);
                if (slotIt != slots.end()) {
                    ReadSlotIdentity(slotIt->second, scratch);
                    detail.nickname = std::move(scratch.nickname);
                    detail.clanTag = std::move(scratch.clanTag);
                    detail.publicId = scratch.publicId;
                }
            }

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
            entity.weaponIndex = detail.weaponIndex;
            entity.isFiring = detail.isFiring;
            entity.isReloading = detail.isReloading;
            entity.hasArmor = detail.hasArmor;
            entity.isKnockedDown = detail.isKnockedDown;
            entity.isKnockedOut = detail.isKnockedOut;
            entity.isSurrendered = detail.isSurrendered;
            entity.isHandcuffed = detail.isHandcuffed;
            entity.reloadPhase = detail.reloadPhase;

            const auto viewIt = views.find(w.entityId);
            if (viewIt != views.end())
                entity.viewPtr = viewIt->second;

            auto& rig = rigCache[w.entityId];

            if (w.needsPose) {
                rig.curLocal.resize(static_cast<std::size_t>(rig.layout.count));
                bool sampled = Skeleton::ProcessPoseData(rig.layout,
                    poseBuf.data() + w.poseOffset, w.poseSize,
                    rig.bones, &rig.points, rig.curLocal.data());

                if (!sampled) {
                    sampled = Skeleton::SampleLayout(rig.layout, DmaReadFresh,
                        rig.bones, &rig.points, rig.curLocal.data());
                    if (!sampled)
                        sampled = Skeleton::SampleLayout(rig.layout, DmaReadFresh,
                            rig.bones, &rig.points, rig.curLocal.data());
                }

                if (!sampled) {
                    if (rig.poseFailures == 0)
                        rig.poseFailureSince = now;
                    ++rig.poseFailures;
                    ++poseFailureCount;
                    // A pose that cannot be read means the matrix array moved
                    // (or the character was pooled). Drop the layout so the
                    // next pass rebuilds; keep the last bones until the
                    // hierarchy has been unreadable for a while, so a single
                    // bad read never blanks the skeleton.
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
                    }
                } else {
                    rig.poseFailures = 0;
                    rig.poseFailureSince = 0;
                    constexpr float kPosEpsSq  = 1.0e-6f;
                    constexpr float kRotEps    = 1.0e-6f;
                    constexpr float kScaleEps  = 1.0e-6f;
                    constexpr std::uint64_t kFreezeMs = 150;

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

                    // Vehicles move fast: smoothing world coordinates creates a lagging trailing skeleton.
                    // Bypass exponential smoothing when inside a vehicle or moving fast (> 0.4m / pass).
                    const bool isFastMoving = entity.inVehicle ||
                        (Distance(rig.lastSampledPos, entity.position) > 0.4f);
                    rig.lastSampledPos = entity.position;

                    // Exponential smoothing on the worker. PoseSmoothAlpha is
                    // the weight of the new sample: 1 publishes the raw pose,
                    // lower values damp the per-frame jitter that partial or
                    // stale DMA reads add to the sample. Doing it here (and
                    // drawing the result directly) means the overlay never
                    // extrapolates past the newest pose, which is what made
                    // the old render-side blend shake.
                    if (!isFastMoving && ConfigInstance.Player.PoseSmoothing && rig.smoothedValid &&
                        rig.smoothedBones.size() == rig.bones.size()) {
                        const float alpha = std::clamp(
                            ConfigInstance.Player.PoseSmoothAlpha, 0.1f, 1.0f);
                        const float inv = 1.0f - alpha;
                        for (std::size_t i = 0; i < rig.bones.size(); ++i) {
                            auto& s = rig.smoothedBones[i];
                            const auto& r = rig.bones[i];
                            s.a.x = s.a.x * inv + r.a.x * alpha;
                            s.a.y = s.a.y * inv + r.a.y * alpha;
                            s.a.z = s.a.z * inv + r.a.z * alpha;
                            s.b.x = s.b.x * inv + r.b.x * alpha;
                            s.b.y = s.b.y * inv + r.b.y * alpha;
                            s.b.z = s.b.z * inv + r.b.z * alpha;
                            s.visible = r.visible;
                        }
                        auto lerpVec = [&](Skeleton::Vec& dst, const Skeleton::Vec& src) {
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
                    } else {
                        rig.smoothedBones = rig.bones;
                        rig.smoothedPoints = rig.points;
                        rig.smoothedValid = true;
                    }
                }
            } else if (!rig.layoutValid && now - rig.fallbackStamp > 1000) {
                rig.fallbackStamp = now;
                // Never replace good bones with an empty result: this fallback
                // runs on a timer, so a single failed read used to blank the
                // skeleton for a full second (a visible strobe).
                auto fallback = Skeleton::BuildSkeleton(cachedGame, cachedBattleModel,
                    entity.entityId, DmaRead);
                if (!fallback.empty()) {
                    rig.bones = std::move(fallback);
                    rig.points = Skeleton::BonePoints{};
                    rig.smoothedValid = false;
                }
            }

            if (ConfigInstance.Player.PoseSmoothing && rig.smoothedValid) {
                entity.bones = rig.smoothedBones;
                entity.bonePoints = rig.smoothedPoints;
            } else {
                entity.bones = rig.bones;
                entity.bonePoints = rig.points;
            }
            entity.visible = rig.animatorActive;

            // Camera-occlusion pass. VisCheck colours each bone by whether the
            // camera can see it; WallCheck also hides the whole character from
            // the aim gate when every bone is occluded. Both need the BVH, so
            // without it the animator fallback above stays in effect and the
            // menu says the occluder file is missing.
            if ((ConfigInstance.Player.VisCheck || ConfigInstance.Player.WallCheck) &&
                g_bvhScene.valid && visCamera.valid &&
                entity.entityId != localEntityId)
            {
                const CollisionBVH::Float3 cam = {
                    visCamera.position.x,
                    visCamera.position.y,
                    visCamera.position.z };
                bool anyVisible = false;
                if (!entity.bones.empty()) {
                    for (auto& seg : entity.bones) {
                        const CollisionBVH::Float3 mid = {
                            (seg.a.x + seg.b.x) * 0.5f,
                            (seg.a.y + seg.b.y) * 0.5f,
                            (seg.a.z + seg.b.z) * 0.5f };
                        seg.visible = !CollisionBVH::IsOccluded(g_bvhScene, cam, mid);
                        if (seg.visible)
                            anyVisible = true;
                    }
                } else {
                    // Fallback when entity lacks skeleton segments (rig not yet built or culled at distance):
                    // Automatically cast rays to torso (y + halfHeight) and head (y + headHeight)
                    const CollisionBVH::Float3 torso = {
                        entity.position.x,
                        entity.position.y + 0.95f,
                        entity.position.z };
                    const CollisionBVH::Float3 head = {
                        entity.position.x,
                        entity.position.y + 1.65f,
                        entity.position.z };

                    bool torsoVis = !CollisionBVH::IsOccluded(g_bvhScene, cam, torso);
                    bool headVis = !CollisionBVH::IsOccluded(g_bvhScene, cam, head);
                    anyVisible = torsoVis || headVis;
                }
                entity.visible = anyVisible;
            }

            if (entity.entityId == localEntityId && localPosition.IsZero() &&
                !entity.position.IsZero()) {
                localPosition = entity.position;
            }

            entities.push_back(std::move(entity));
        }
    }

    // The view dictionary is refreshed at most every 400 ms. Match it against
    // this pass's fresh player positions so the pairing is ready for the next
    // passes; doing it before the entity loop would match against a list that
    // was just cleared.
    if (viewsRefreshed)
    {
        viewsRefreshed = false;
        MatchViewsToEntities();
    }

    const std::uint64_t passNow = GetTickCount64();

    // World and inventory reading runs on dedicated g_worldWorkerThread
    // so this player pose pass runs at full speed without latency spikes.

    // A root change here means this pass rebuilt the scene identity. Bump the
    // generation so memory aim blocks writes while the new objects settle.
    NoteSessionState();

    PruneCaches();
    failureCount = 0;
}

void Update()
{
    if (!g_bvhScene.valid || ConfigInstance.Player.BvhPath != g_bvhLoadedPath)
    {
        TryLoadBvh(false);
    }

    UpdateInternal();
    PublishSnapshot();
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

void SetReadCatalogEnabled(bool enabled) { readCatalog.store(enabled); }

void RefreshCamera()
{
    // Fast path: when the native camera was valid last frame, skip the
    // managed read entirely and scatter-read just view+projection in one
    // device transaction (~2ms instead of ~8ms).
    if (ConfigInstance.Player.NativeEspCamera) {
        bool nativeWasValid;
        {
            std::lock_guard<std::mutex> lock(g_nativeCameraMutex);
            nativeWasValid = g_nativeViewProjectionValid;
        }
        if (nativeWasValid) {
            CameraFinder::CameraData fast = CameraFinder::GetCameraMatricesFast();
            if (fast.valid) {
                std::lock_guard<std::mutex> lock(g_nativeCameraMutex);
                g_nativeViewProjectionValid = true;
                g_nativeView = fast.view;
                g_nativeProjection = fast.projection;
                g_nativePositionError = -1.f;
                g_nativeForwardDot = -1.f;
                return;
            }
        }
    }

    // Full path: read managed camera, then validate native against it.
    RobRunCamera::CameraState managed{};
    const std::uintptr_t model = g_renderCameraModel.load(std::memory_order_acquire);
    if (model) {
        managed = RobRunCamera::Read(model);
        if (managed.valid) {
            const float nativeFov = CameraFinder::ReadNativeFov();
            if (nativeFov > 0.f)
                managed.verticalFov = nativeFov;
            std::lock_guard<std::mutex> lock(g_freshCameraMutex);
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

    CameraFinder::CameraData native;
    if (ConfigInstance.Player.NativeEspCamera)
        native = CameraFinder::GetCameraMatricesValidated(
            managed.position, managed.forward, managed.verticalFov);

    {
        std::lock_guard<std::mutex> lock(g_nativeCameraMutex);
        g_nativeViewProjectionValid = native.valid;
        if (native.valid) {
            g_nativeView = native.view;
            g_nativeProjection = native.projection;
        }
        g_nativePositionError = native.positionError;
        g_nativeForwardDot = native.forwardDot;
    }
}

bool GetViewProjection(Matrix4x4& out, int screenWidth, int screenHeight)
{
    if (screenWidth <= 0 || screenHeight <= 0) return false;

    {
        std::lock_guard<std::mutex> lock(g_nativeCameraMutex);
        if (g_nativeViewProjectionValid) {
            // Keep the rendered camera's view and vertical FOV, but rebuild the
            // horizontal scale for the overlay surface. When the overlay and
            // the game render target share an aspect (borderless fullscreen)
            // this is bit-for-bit the matrix the game used.
            Matrix4x4 projection = g_nativeProjection;
            const float aspect =
                static_cast<float>(screenWidth) / static_cast<float>(screenHeight);
            const float yScale = projection.matrix[1][1];
            if (std::isfinite(yScale) && yScale > 0.f && aspect > 0.f)
                projection.matrix[0][0] = yScale / aspect;
            out = g_nativeView * projection;
            return true;
        }
    }

    RobRunCamera::CameraState camera;
    {
        std::lock_guard<std::mutex> lock(g_freshCameraMutex);
        camera = g_freshCamera;
    }
    if (!camera.valid) {
        const auto snapshot = GetSnapshot();
        if (!snapshot) return false;
        camera = snapshot->camera;
    }
    if (!camera.valid) return false;

    const float aspect = static_cast<float>(screenWidth) / static_cast<float>(screenHeight);
    return RobRunCamera::BuildViewProjection(camera, aspect, out);
}

bool GetFallbackViewProjection(Matrix4x4& out)
{
    const auto snapshot = GetSnapshot();
    if (!snapshot || !snapshot->fallbackViewProjectionValid) return false;
    out = snapshot->fallbackViewProjection;
    return true;
}

bool ResolveViewProjection(Matrix4x4& out, int screenWidth, int screenHeight)
{
    if (screenWidth <= 0 || screenHeight <= 0) return false;
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
    {
        std::lock_guard<std::mutex> lock(g_nativeCameraMutex);
        if (g_nativeViewProjectionValid) return "native";
    }
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
    {
        std::lock_guard<std::mutex> lock(g_nativeCameraMutex);
        debug.nativeValid = g_nativeViewProjectionValid;
        debug.positionError = g_nativePositionError;
        debug.forwardDot = g_nativeForwardDot;
    }
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
    return RobRunCamera::WorldToScreen(viewProjection, world, screenWidth, screenHeight, out);
}

BvhStatus GetBvhStatus()
{
    BvhStatus s;
    s.loaded = g_bvhScene.valid;
    s.nodeCount = (uint32_t)g_bvhScene.nodes.size();
    s.triCount = (uint32_t)g_bvhScene.tris.size();
    s.path = g_bvhScene.loadedPath;
    return s;
}

void ReloadBvh()
{
    TryLoadBvh(true);
}

} // namespace RobRunReader
