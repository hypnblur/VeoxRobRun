#include "pch.h"
#include "RobRunReader.h"
#include "RobRunMemory.h"
#include "Il2Cpp.h"
#include "Memory.h"
#include "CameraFinder.h"
#include "CollisionBVH.h"
#include "ConfigInstance.h"
#include "DebugLog.h"
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

static void MarkSessionChanged()
{
    ++sessionGeneration;
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
    g_slotIdentityCache.clear();
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
};
static std::unordered_map<std::int32_t, RigCacheEntry> rigCache;

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
// World pose parsed from the native view matrix. Aim uses it as its angle and
// occlusion origin so the origin and the projection come from one camera read.
static Vector3 g_nativeCameraPosition{};
static Vector3 g_nativeCameraForward{};
static bool g_nativeCameraPoseValid = false;

static CollisionBVH::Scene g_bvhScene;
static std::string g_bvhLoadedPath;
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
    snapshot->entities = entities;
    {
        std::lock_guard<std::mutex> lock(g_worldMutex);
        snapshot->world = g_worldState;
        snapshot->plots = g_plotState;
        snapshot->inventory = g_inventoryState;
    }
    snapshot->camera = cameraState;
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
    RestoreDefaultCrosshair();
    g_workerStop.store(true, std::memory_order_relaxed);
    g_worldWorkerStop.store(true, std::memory_order_relaxed);
    if (g_workerThread.joinable())
        g_workerThread.join();
    if (g_worldWorkerThread.joinable())
        g_worldWorkerThread.join();

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
    {
        std::lock_guard<std::mutex> lock(g_freshCameraMutex);
        g_freshCamera = RobRunCamera::CameraState{};
    }
    {
        std::lock_guard<std::mutex> lock(g_nativeCameraMutex);
        g_nativeViewProjectionValid = false;
        g_nativeCameraPoseValid = false;
        g_nativeCameraPosition = Vector3{};
        g_nativeCameraForward = Vector3{};
    }
    gaBase = 0;
    upBase = 0;
}

void Init(std::uintptr_t gameAssemblyBase, std::uintptr_t unityPlayerBase)
{
    DLOG("RobRunReader::Init(ga=0x%llx, up=0x%llx)", (unsigned long long)gameAssemblyBase, (unsigned long long)unityPlayerBase);
    Shutdown();
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
    DLOG("RobRunReader::Init: BVH done, valid=%d", g_bvhScene.valid);

    {
        std::lock_guard<std::mutex> lock(g_snapshotMutex);
        g_snapshot = std::make_shared<Snapshot>();
    }

    DLOG("RobRunReader::Init: starting worker threads");
    g_workerStop.store(false, std::memory_order_relaxed);
    g_workerThread = std::thread([] {
        DLOG("Worker thread started");
        while (!g_workerStop.load(std::memory_order_relaxed)) {
            Update();
            // Adaptive sleep: 1 ms when entities are active to keep snapshots fresh (50-60 Hz),
            // or 10 ms during empty lobbies / scene transitions.
            const int sleepMs = (cachedBattleModel > 0x10000 && !entities.empty()) ? 1 : 10;
            for (int i = 0; i < sleepMs && !g_workerStop.load(std::memory_order_relaxed); ++i)
                Sleep(1);
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
            for (int i = 0; i < 25 && !g_worldWorkerStop.load(std::memory_order_relaxed); ++i)
                Sleep(10);
        }
    });

    DLOG("RobRunReader::Init: complete, both workers launched");
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

    const auto it = g_roleCache.find(roleWrapper);
    if (it != g_roleCache.end() && it->second.resolved) {
        if (!it->second.roleName.empty()) entity.roleName = it->second.roleName;
        if (it->second.roleId != 0) entity.role = it->second.roleId;
        return;
    }

    int roleId = 0;
    std::string roleName;
    if (ResolveRoleFromObject(roleWrapper, roleId, roleName)) {
        if (!roleName.empty()) entity.roleName = roleName;
        if (roleId != 0) entity.role = roleId;
        if (!entity.roleName.empty()) {
            g_roleCache[roleWrapper] = RoleCacheEntry{ roleId, roleName, true };
            return;
        }
    }

    const std::uintptr_t innerPtr = ReadPtr(roleWrapper + rr::ReferenceData::Value);
    if (innerPtr > 0x10000 && Il2Cpp::ClassOf(innerPtr) != 0) {
        if (roleId != 0 || !roleName.empty())
            g_roleCache[roleWrapper] = RoleCacheEntry{ roleId, roleName, true };
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

    if (entity.role != 0 || !entity.roleName.empty()) {
        g_roleCache[roleWrapper] = RoleCacheEntry{ entity.role, entity.roleName, true };
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

    if (entity.roleName.empty() && entity.role == 0)
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

static void UpdateInternal()
{
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
                if (++gameResolveFails >= 300) {
                    DLOG("GameRoot: sustained failure (%d attempts), attempting CR3 fix", gameResolveFails);
                    gameResolveFails = 0;
                    TargetProcess.FixCr3();
                }
            } else {
                gameResolveFails = 0;
            }
        } else {
            --gameRetryCountdown;
        }
    } else {
        gameResolveFails = 0;
    }

    if (uiCount <= 3) DLOG("UpdateInternal #%d: game=0x%llx", uiCount, (unsigned long long)cachedGame);

    if (!cachedBattleModel)
        cachedBattleModel = ResolveBattleModel(cachedGame);

    if (!cachedBattleModel) {
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
        NoteSessionState();
        ++failureCount;
        DLOG("ReaderRoot: BattleModel lost, failures=%u game=0x%llx",
             failureCount, (unsigned long long)cachedGame);
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
                    std::lock_guard<std::mutex> lk(locationMutex);
                    if (id != cachedLocationId)
                        DLOG("Location: '%s' -> '%s'", cachedLocationId.c_str(), id.c_str());
                    cachedLocationId = std::move(id);
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
                if (!fresh.empty())
                    cachedSlots.swap(fresh);
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

    if (uiCount <= 3) DLOG("UpdateInternal #%d: pre-entity, battleModel=0x%llx, battleEntities=0x%llx",
        uiCount, (unsigned long long)cachedBattleModel, (unsigned long long)cachedBattleEntities);

    const std::uintptr_t entityDictionary = ReadPtr(
        cachedBattleEntities + rr::BattleEntitiesModel::EntityDict);
    std::vector<RobRunMemory::DictEntry> entityEntries;
    static int entityDictFailures = 0;
    if (RobRunMemory::ReadDictInt(entityDictionary, entityEntries)) {
        entityDictFailures = 0;
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
            auto sc = TargetProcess.CreateScatterHandleCached();
            if (sc) {
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
                TargetProcess.CloseScatterHandle(sc);
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
                        sizeof(TransformBlock)))
                {
                    w.positionValid = false;
                    continue;
                }
            }
            w.positionValid = RobRunMemory::Finite(w.block.position);
        }

        if (uiCount <= 3) DLOG("UpdateInternal #%d: phase2 done, %zu work items", uiCount, work.size());

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
                rig.smoothedWorldMatrices.clear();
                rig.lastSampledPos = Vector3{};
                rig.lastMotionTick = 0;
                rig.animatorActive = true;
            }

            // Skeleton layout rebuild is wrapped in try/catch because the
            // DMA reads through the entity view hierarchy can trigger AVs
            // on some hardware setups. With /EHa this catch handles SEH.
            try {
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
                    rig.smoothedValid = false;
                } else {
                    if (rig.poseFailureSince == 0)
                        rig.poseFailureSince = now;
                    if (now - rig.poseFailureSince >= 2500) {
                        rig.bones.clear();
                        rig.points = Skeleton::BonePoints{};
                        rig.smoothedValid = false;
                        rig.smoothedWorldMatrices.clear();
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
            std::uintptr_t vehicleWrapper{};
            std::uintptr_t reloadState{};
            std::uintptr_t reloadEndTsW{};
            std::uintptr_t kdEndTsW{};
            std::uintptr_t koPhase{};
            std::uintptr_t koEndTsW{};
            std::uint8_t   koSurrendered{};
            std::uint8_t   koHandcuffed{};
            std::uintptr_t weaponCurrent{};
            // phase 3: third level + blocks
            struct { std::uintptr_t hw; std::uintptr_t dw; std::int32_t maxAmt; std::int32_t critHp; } hb{};
            std::uintptr_t staminaW{};
            std::uintptr_t armorW{};
            std::uintptr_t commonRole{};
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
            auto sc = TargetProcess.CreateScatterHandleCached();
            if (sc) {
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

                // Phase 2: second-level pointers
                for (int i = 0; i < seCount; ++i) {
                    auto& s = se[i];
                    if (s.privateModel > 0x10000) {
                        TargetProcess.AddScatterReadRequest(sc, s.privateModel + rr::BattleCharacterPrivateModel::HealthModel, &s.healthModel, 8);
                        TargetProcess.AddScatterReadRequest(sc, s.privateModel + rr::BattleCharacterPrivateModel::InitArmorModel, &s.initArmorModel, 8);
                        TargetProcess.AddScatterReadRequest(sc, s.privateModel + rr::BattleCharacterPrivateModel::Stamina, &s.staminaModel, 8);
                    }
                    if (s.data > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.data + rr::BattleData::Common, &s.common, 8);
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
                        TargetProcess.AddScatterReadRequest(sc, s.knockout + rr::KnockoutModel::IsHandcuffed, &s.koHandcuffed, 1);
                    }
                    if (s.itemInHands > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.itemInHands + rr::ItemInHandsModel::Current, &s.weaponCurrent, 8);
                }
                if (!TargetProcess.ExecuteScatterReadCached(sc))
                    DLOG("ScatterFail: detail phase 2 (seCount=%d)", seCount);

                // Phase 3: third-level pointers + block reads
                for (int i = 0; i < seCount; ++i) {
                    auto& s = se[i];
                    if (s.healthModel > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.healthModel + rr::HealthModel::Health, &s.hb, sizeof(s.hb));
                    if (s.staminaModel > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.staminaModel + rr::StaminaModel::Amount, &s.staminaW, 8);
                    if (s.initArmorModel > 0x10000)
                        TargetProcess.AddScatterReadRequest(sc, s.initArmorModel + rr::InitArmorModel::Amount, &s.armorW, 8);
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
                TargetProcess.CloseScatterHandle(sc);
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

                // Knockdown
                det.isKnockedDown = false;
                if (battleNowMs > 0 && s.kdEndTs > kSentinelTs && s.kdEndTs > battleNowMs &&
                    s.kdLeaveActive == 0)
                    det.isKnockedDown = true;

                // Knockout
                det.isKnockedOut = false;
                if (s.koPhaseDesc > 0x10000)
                    det.isKnockedOut = true;
                else if (battleNowMs > 0 && s.koEndTs > kSentinelTs && s.koEndTs > battleNowMs)
                    det.isKnockedOut = true;
                det.isSurrendered = s.koSurrendered != 0;
                det.isHandcuffed = s.koHandcuffed != 0;

                if (det.isKnockedOut && s.entityId == localEntityId &&
                    ConfigInstance.Aim.IgnoreKnock && s.knockout > 0x10000) {
                    std::uintptr_t zero = 0;
                    TargetProcess.Write(
                        s.knockout + rr::KnockoutModel::Phase, zero);
                }

                // Weapon (uses cache — only reads strings on miss)
                if (s.weaponDesc > 0x10000) {
                    const auto wit = g_weaponDescCache.find(s.weaponDesc);
                    if (wit != g_weaponDescCache.end() && wit->second.valid) {
                        det.weaponName = wit->second.name;
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
                        det.weaponIndex = idx >= 0 ? idx : -1;
                        if (!wn.empty() || idx >= 0)
                            g_weaponDescCache[s.weaponDesc] = WeaponDescCache{ wn, det.weaponIndex, true };
                    }
                } else {
                    det.weaponName.clear();
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
            // Built once here (worker pass) instead of on every ESP frame.
            entity.nicknameUtf8 = RobRunMemory::WideToUtf8(entity.nickname);
            entity.clanTagUtf8 = RobRunMemory::WideToUtf8(entity.clanTag);
            entity.weaponNameUtf8 = RobRunMemory::WideToUtf8(entity.weaponName);
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

                // Sample into temporaries so a failed read never
                // overwrites the last good frame (async DMA flicker fix).
                std::vector<Skeleton::BoneSegment> tmpBones;
                Skeleton::BonePoints tmpPoints{};
                std::vector<Skeleton::BoneLocal> tmpLocal(static_cast<std::size_t>(rig.layout.count));
                std::vector<Matrix4x4> tmpWorld(static_cast<std::size_t>(rig.layout.count));

                Skeleton::PoseStats pst{};
                bool sampled = Skeleton::ProcessPoseData(rig.layout,
                    poseBuf.data() + w.poseOffset, w.poseSize,
                    tmpBones, &tmpPoints, tmpLocal.data(),
                    tmpWorld.data(), &pst);

                if (sampled) {
                    ++poseScatterOkAcc;
                } else {
                    ++poseScatterFailAcc;
                    pst = {};
                    tmpLocal.assign(static_cast<std::size_t>(rig.layout.count), Skeleton::BoneLocal{});
                    tmpWorld.assign(static_cast<std::size_t>(rig.layout.count), Matrix4x4{});
                    sampled = Skeleton::SampleLayout(rig.layout, DmaReadFresh,
                        tmpBones, &tmpPoints, tmpLocal.data(),
                        tmpWorld.data(), &pst);
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
                    }
                } else {
                    // Commit the good frame.
                    rig.bones = std::move(tmpBones);
                    rig.points = tmpPoints;
                    rig.curLocal = std::move(tmpLocal);
                    rig.worldMatrices = std::move(tmpWorld);
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
                    } else {
                        rig.smoothedBones = rig.bones;
                        rig.smoothedPoints = rig.points;
                        rig.smoothedWorldMatrices = rig.worldMatrices;
                        rig.smoothedValid = true;
                    }
                }
            } else if (!rig.layoutValid && now - rig.fallbackStamp > 1000) {
                rig.fallbackStamp = now;
                if (uiCount <= 3) DLOG("UpdateInternal #%d: cp=D2 (pre-BuildSkeleton)", uiCount);
                // Never replace good bones with an empty result: this fallback
                // runs on a timer, so a single failed read used to blank the
                // skeleton for a full second (a visible strobe).
                auto fallback = Skeleton::BuildSkeleton(cachedGame, cachedBattleModel,
                    entity.entityId, DmaRead);
                if (uiCount <= 3) DLOG("UpdateInternal #%d: cp=D3 (post-BuildSkeleton, %zu bones)", uiCount, fallback.size());
                if (!fallback.empty()) {
                    rig.bones = std::move(fallback);
                    rig.points = Skeleton::BonePoints{};
                    rig.smoothedValid = false;
                    rig.smoothedWorldMatrices.clear();
                }
            }

            if (ConfigInstance.Player.PoseSmoothing && rig.smoothedValid) {
                entity.bones = rig.smoothedBones;
                entity.bonePoints = rig.smoothedPoints;
            } else {
                entity.bones = rig.bones;
                entity.bonePoints = rig.points;
            }
            } catch (...) {
                static int catchCount = 0;
                if (catchCount++ < 10) DLOG("UpdateInternal: skeleton catch(...) fired for entity %d", entity.entityId);
            }
            if (uiCount <= 3) DLOG("UpdateInternal #%d: cp=E (bones done)", uiCount);
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
            entity.visible = rig.animatorActive;

            // Camera-occlusion pass. VisCheck colours each bone by whether the
            // camera can see it; WallCheck also hides the whole character from
            // the aim gate when every bone is occluded. Both need the BVH, so
            // without it the animator fallback above stays in effect and the
            // menu says the occluder file is missing.
            // Distance gate per item 9:
            const bool haveLocalPos = RobRunMemory::Finite(localPosition) && !localPosition.IsZero();
            const float entDist = haveLocalPos ? Distance(entity.position, localPosition) : 0.f;
            if (ConfigInstance.Player.VisMaxDistance > 0.f && entDist > ConfigInstance.Player.VisMaxDistance) {
                entity.visible = false;
                for (auto& seg : entity.bones) {
                    seg.visible = false;
                }
            }
            else if ((ConfigInstance.Player.VisCheck || ConfigInstance.Player.WallCheck || ConfigInstance.Aim.VisibleBonesOnly) &&
                g_bvhScene.valid && visCamera.valid &&
                entity.entityId != localEntityId)
            {
                const CollisionBVH::Float3 cam = {
                    visCamera.position.x,
                    visCamera.position.y,
                    visCamera.position.z };

                bool anyVisible = false;
                const int rayCount = ConfigInstance.Player.VisRayCount;

                if (rayCount <= 2 || entity.bones.empty()) {
                    // Level 1: 2 Rays (Torso + Head). Lowest CPU load.
                    const CollisionBVH::Float3 torso = {
                        entity.position.x,
                        entity.position.y + 0.95f,
                        entity.position.z };
                    const CollisionBVH::Float3 head = {
                        entity.position.x,
                        entity.position.y + 1.65f,
                        entity.position.z };

                    bool torsoVis = !CollisionBVH::IsOccluded(g_bvhScene, cam, torso, &dynCopy, carCabinSolid);
                    bool headVis = !CollisionBVH::IsOccluded(g_bvhScene, cam, head, &dynCopy, carCabinSolid);
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

                    bool vHead   = !CollisionBVH::IsOccluded(g_bvhScene, cam, pHead, &dynCopy, carCabinSolid);
                    bool vChest  = !CollisionBVH::IsOccluded(g_bvhScene, cam, pChest, &dynCopy, carCabinSolid);
                    bool vPelvis = !CollisionBVH::IsOccluded(g_bvhScene, cam, pPelvis, &dynCopy, carCabinSolid);
                    bool vLHand  = !CollisionBVH::IsOccluded(g_bvhScene, cam, pLHand, &dynCopy, carCabinSolid);
                    bool vRHand  = !CollisionBVH::IsOccluded(g_bvhScene, cam, pRHand, &dynCopy, carCabinSolid);
                    bool vFeet   = !CollisionBVH::IsOccluded(g_bvhScene, cam, pFeet, &dynCopy, carCabinSolid);

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
                        seg.visible = !CollisionBVH::IsOccluded(g_bvhScene, cam, mid, &dynCopy, carCabinSolid);
                        if (seg.visible)
                            anyVisible = true;
                    }
                }
                entity.visible = anyVisible;
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
        viewsRefreshed = false;
        MatchViewsToEntities();
    }

    const std::uint64_t passNow = GetTickCount64();

    // World and inventory reading runs on dedicated g_worldWorkerThread
    // so this player pose pass runs at full speed without latency spikes.

    if (cachedGame > 0x10000) {
        if (ConfigInstance.Aim.HideCrosshair)
            HideDefaultCrosshair(cachedGame);
        else
            RestoreDefaultCrosshair();
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
        if (!g_bvhScene.valid || ConfigInstance.Player.BvhPath != g_bvhLoadedPath)
        {
            TryLoadBvh(false);
        }

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

void RefreshCamera()
{
    // Both the render thread and the aim worker call this. Each call is a DMA
    // device transaction, and the two consumers only need the newest camera, so
    // collapse calls that land within 3 ms of each other. The loser simply uses
    // the camera the winner just published.
    static std::atomic<std::int64_t> lastRefreshUs{ 0 };
    const std::int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::int64_t previous = lastRefreshUs.load(std::memory_order_relaxed);
    if (nowUs - previous < 3000) return;
    if (!lastRefreshUs.compare_exchange_strong(previous, nowUs, std::memory_order_relaxed))
        return;

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
                {
                    std::lock_guard<std::mutex> lock(g_nativeCameraMutex);
                    g_nativeViewProjectionValid = true;
                    g_nativeView = fast.view;
                    g_nativeProjection = fast.projection;
                    g_nativePositionError = -1.f;
                    g_nativeForwardDot = -1.f;
                    // Keep the aim/occlusion origin glued to the rendered
                    // camera too, not just the projection.
                    g_nativeCameraPoseValid = !fast.position.IsZero();
                    if (g_nativeCameraPoseValid) {
                        g_nativeCameraPosition = fast.position;
                        g_nativeCameraForward = fast.forward;
                    }
                }
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
        g_nativeCameraPoseValid = native.valid && !native.position.IsZero();
        if (g_nativeCameraPoseValid) {
            g_nativeCameraPosition = native.position;
            g_nativeCameraForward = native.forward;
        }
    }
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
    Vector3 nativePosition{};
    Vector3 nativeForward{};
    bool haveNativePose = false;
    {
        std::lock_guard<std::mutex> lock(g_nativeCameraMutex);
        haveNativePose = g_nativeCameraPoseValid;
        nativePosition = g_nativeCameraPosition;
        nativeForward = g_nativeCameraForward;
    }

    out = managed;
    if (haveNativePose) {
        out.position = nativePosition;
        out.forward = nativeForward;
        out.valid = true;
    }
    return out.valid;
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
            if (std::isfinite(yScale) && yScale > 0.01f && aspect > 0.01f)
                projection.matrix[0][0] = yScale / aspect;
            Matrix4x4 combined = g_nativeView * projection;
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
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (!std::isfinite(snapshot->fallbackViewProjection.matrix[r][c])) return false;
        }
    }
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
    if (screenWidth <= 0 || screenHeight <= 0) return false;
    if (!RobRunMemory::Finite(world)) return false;
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

const CollisionBVH::Scene& GetBvhScene()
{
    return g_bvhScene;
}

bool IsOccluded(const Vector3& origin, const Vector3& target, float tMin, bool checkDynamic)
{
    if (!g_bvhScene.valid) return false;
    const CollisionBVH::Float3 o{ origin.x, origin.y, origin.z };
    const CollisionBVH::Float3 t{ target.x, target.y, target.z };
    CollisionBVH::DynamicOccluders dynCopy;
    const CollisionBVH::DynamicOccluders* dyn = nullptr;
    if (checkDynamic) {
        std::lock_guard<std::mutex> lock(g_dynOccludersMutex);
        dynCopy = g_dynOccluders;
        dyn = &dynCopy;
    }
    return CollisionBVH::IsOccluded(g_bvhScene, o, t, dyn, ConfigInstance.Player.CarCabinSolid, tMin);
}

bool IsCameraOccluded(const Vector3& target, float tMin, bool checkDynamic)
{
    if (!g_bvhScene.valid) return false;
    RobRunCamera::CameraState cam = cameraState;
    RobRunCamera::CameraState fresh{};
    if (GetFreshCamera(fresh)) cam = fresh;
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
    return CollisionBVH::IsOccluded(g_bvhScene, o, t, dyn, ConfigInstance.Player.CarCabinSolid, tMin);
}

} // namespace RobRunReader
