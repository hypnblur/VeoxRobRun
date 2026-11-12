#include "pch.h"
#include "RobRunReader.h"
#include "RobRunMemory.h"
#include "Il2Cpp.h"
#include "Memory.h"
#include "CameraFinder.h"
#include "CollisionBVH.h"
#include "ConfigInstance.h"
#include <atomic>
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

static void MarkSessionChanged()
{
    ++sessionGeneration;
    sessionChangedMs = GetTickCount64();
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
    std::uint64_t stamp = 0;
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
    std::wstring nickname;
    std::wstring clanTag;
};
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
    snapshot->sessionGeneration = sessionGeneration;
    snapshot->sessionChangedMs = sessionChangedMs;
    for (const auto& entity : entities) {
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
    if (g_workerThread.joinable())
        g_workerThread.join();
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

    if (!ConfigInstance.Player.BvhPath.empty() &&
        ConfigInstance.Player.BvhPath != g_bvhLoadedPath)
    {
        g_bvhScene = CollisionBVH::Scene{};
        if (CollisionBVH::Load(ConfigInstance.Player.BvhPath, g_bvhScene))
            g_bvhLoadedPath = ConfigInstance.Player.BvhPath;
        else
            g_bvhLoadedPath.clear();
    }

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
}

static void ReadRoleAndVehicle(std::uintptr_t characterModel, EntityData& entity)
{
    if (characterModel <= 0x10000) return;

    const std::uintptr_t roleWrapper = ReadPtr(
        characterModel + rr::BattleCharacterModel::PlayerRole);
    std::int32_t role = 0;
    if (Il2Cpp::ReadPrimitiveInt(roleWrapper, role))
        entity.role = role;

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
// One traversal covers role, movement state, death and vehicle id.
static void ReadCommonState(std::uintptr_t characterModel, EntityData& entity)
{
    if (characterModel <= 0x10000) return;
    const std::uintptr_t data = ReadPtr(
        characterModel + rr::BattleCharacterModel::Data);
    if (data <= 0x10000) return;
    const std::uintptr_t common = ReadPtr(data + rr::BattleData::Common);
    if (common <= 0x10000) return;

    std::int32_t role = 0;
    if (Il2Cpp::ReadPrimitiveInt(ReadPtr(common + rr::CommonModel::Role), role))
        entity.role = role;

    bool flag = false;
    if (Il2Cpp::ReadPrimitiveBool(ReadPtr(common + rr::CommonModel::IsSprinting), flag))
        entity.isSprinting = flag;
    if (Il2Cpp::ReadPrimitiveBool(ReadPtr(common + rr::CommonModel::IsCrouching), flag))
        entity.isCrouching = flag;
    if (Il2Cpp::ReadPrimitiveBool(ReadPtr(common + rr::CommonModel::IsDead), flag))
        entity.isDead = flag;

    std::int32_t vehicleId = 0;
    if (Il2Cpp::ReadPrimitiveInt(ReadPtr(common + rr::CommonModel::VehicleEntityId), vehicleId)) {
        entity.inVehicle = vehicleId > 0;
        entity.vehicleId = vehicleId > 0 ? vehicleId : 0;
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

    // Legacy: the dictionary value is BattleCharacterSlotModel, which points at
    // its data object at +0x10.
    if (entity.nickname.empty() || entity.clanTag.empty()) {
        const std::uintptr_t slotData = ReadPtr(
            slotValue + rr::BattleCharacterSlotModel::_data);
        if (slotData > 0x10000 && slotData != slotValue) {
            if (entity.nickname.empty())
                ReadSlotString(slotData, rr::BattleCharacterSlotData::Nickname, entity.nickname);
            if (entity.clanTag.empty())
                ReadSlotString(slotData, rr::BattleCharacterSlotData::ClanTag, entity.clanTag);
        }
    }
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

    // Re-resolve every pass instead of only when zero. A level change destroys
    // and recreates the camera object; keeping the first pointer made memory
    // aim keep writing into freed/reused memory, which crashed the game.
    {
        const std::uintptr_t cameraCandidate = ReadPtr(
            cachedBattleModel + rr::BattleModel::BattleCameraModel);
        cachedCameraModel =
            (cameraCandidate > 0x10000 &&
             IsClass(cameraCandidate, "BattleCameraModel"))
            ? cameraCandidate : 0;
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

            // Resolve LookDirectionModel for multi-model aim writes:
            // BattleCharacterModel -> Data(0x40) -> CommonModel::LookDirection(0x30)
            const std::uintptr_t dataModel = ReadPtr(
                cachedLocalCharacter + rr::BattleCharacterModel::Data);
            if (dataModel > 0x10000)
                cachedLookDirectionModel = ReadPtr(
                    dataModel + rr::CommonModel::LookDirection);
            else
                cachedLookDirectionModel = 0;

            // BCM+0x70 LookDirectionModel (has _value, _yaw, _pitch)
            cachedLookDirDirect = ReadPtr(cachedLocalCharacter + rr::BattleCharacterModel::LookDirection);
            if (cachedLookDirDirect <= 0x10000) cachedLookDirDirect = 0;

            // BattleModel+0x40 PlayerRotationInput
            if (cachedBattleModel > 0x10000)
                cachedPlayerRotationInput = ReadPtr(cachedBattleModel + rr::BattleModel::PlayerRotationInput);
            else
                cachedPlayerRotationInput = 0;
            if (cachedPlayerRotationInput <= 0x10000) cachedPlayerRotationInput = 0;
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

    std::unordered_map<std::int32_t, std::uintptr_t> views;
    if (cachedUpdater > 0x10000) {
        const std::uintptr_t viewDictionary = ReadPtr(
            cachedUpdater + rr::BattleEntitiesUpdater::_characterEntityViews);
        std::vector<RobRunMemory::DictEntry> viewEntries;
        if (RobRunMemory::ReadDictInt(viewDictionary, viewEntries)) {
            views.reserve(viewEntries.size());
            for (const auto& entry : viewEntries)
                views[entry.key] = entry.value;
        }
    }

    std::unordered_map<std::int32_t, std::uintptr_t> slots;
    std::vector<RobRunMemory::DictEntry> slotEntries;
    bool slotsRead = false;

    // New offset set: BattleModel+0x248 -> slots model +0x18 -> dictionary.
    // Both the model offset and the dictionary offset have legacy variants, so
    // try the new pair first and fall back without losing older sessions.
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

    // Legacy model offset.
    if (!slotsRead) {
        const std::uintptr_t legacyModel = ReadPtr(
            cachedBattleModel + rr::BattleModel::CharacterSlotsModelLegacy);
        if (legacyModel > 0x10000) {
            std::uintptr_t slotDictionary = ReadPtr(
                legacyModel + rr::CharacterSlotsModel::SlotDictLegacy);
            slotsRead = RobRunMemory::ReadDictInt(slotDictionary, slotEntries);
        }
    }

    // Legacy fallback: BattleEntitiesModel+0x80 -> root +0x18 -> dictionary.
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
        slots.reserve(slotEntries.size());
        for (const auto& entry : slotEntries)
            slots[entry.key] = entry.value;
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
        // Phase 1: Resolve characterModel and transform for each entity.
        // These pointer chains are mostly cached, so sequential is fine.
        // ---------------------------------------------------------------
        std::vector<EntityWork> work;
        work.reserve(entityEntries.size());

        for (const auto& entry : entityEntries) {
            const std::uintptr_t entityModel = entry.value;
            if (entityModel <= 0x10000) continue;

            std::uintptr_t characterModel = ReadPtr(
                entityModel + rr::BattleCharacterEntityModel::ClientModel);

            const auto cachedDetail = detailCache.find(entry.key);
            const bool clientModelKnown = characterModel > 0x10000 &&
                cachedDetail != detailCache.end() &&
                cachedDetail->second.characterModel == characterModel;
            if (!clientModelKnown && !IsClass(characterModel, "BattleCharacterModel")) {
                const std::uintptr_t simulated = ReadPtr(
                    entityModel + rr::BattleCharacterEntityModel::SimulatedModel);
                if (IsClass(simulated, "BattleCharacterModel"))
                    characterModel = simulated;
                else if (IsClass(entityModel, "BattleCharacterModel"))
                    characterModel = entityModel;
                else
                    characterModel = 0;
            }
            if (characterModel <= 0x10000) continue;

            const std::uintptr_t transform = ReadPtr(
                characterModel + rr::BattleCharacterModel::CharacterTransformModel);
            if (transform <= 0x10000) continue;

            EntityWork w{};
            w.entityId = entry.key;
            w.characterModel = characterModel;
            w.transform = transform;
            w.blockAddress = transform + rr::CharacterTransformModel::Position;
            work.push_back(w);
        }

        // ---------------------------------------------------------------
        // Phase 2: Scatter-read all position blocks in one DMA call.
        // ---------------------------------------------------------------
        if (!work.empty()) {
            auto scatter = TargetProcess.CreateScatterHandle();
            if (scatter) {
                for (auto& w : work)
                    TargetProcess.AddScatterReadRequest(scatter,
                        w.blockAddress, &w.block, sizeof(TransformBlock));
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
        // Phase 3: Build entity data, refresh details, resolve layouts,
        //          and collect pose-read requests.
        // ---------------------------------------------------------------
        const std::uint64_t now = GetTickCount64();

        std::vector<unsigned char> poseBuf;
        std::size_t poseTotal = 0;

        for (auto& w : work) {
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
                rig.lastMotionTick = 0;
                rig.animatorActive = true;
            }

            const bool dueForRebuild = !rig.layoutValid
                ? (rig.layoutStamp == 0 || now - rig.layoutStamp >= 250)
                : (now - rig.layoutStamp >= 2000);
            if (dueForRebuild) {
                rig.layoutStamp = now;
                rig.layoutValid = viewPtr > 0x10000 &&
                    Skeleton::BuildLayoutFromView(viewPtr, DmaRead, rig.layout);
                if (!rig.layoutValid) {
                    rig.bones.clear();
                    rig.points = Skeleton::BonePoints{};
                }
            }

            if (rig.layoutValid) {
                const std::size_t sz = Skeleton::PoseDataSize(rig.layout);
                w.needsPose = true;
                w.poseOffset = poseTotal;
                w.poseSize = sz;
                poseTotal += sz;
            }
        }

        // ---------------------------------------------------------------
        // Phase 4: Scatter-read all pose matrices in one DMA call.
        // ---------------------------------------------------------------
        poseBuf.resize(poseTotal, 0);

        if (poseTotal > 0) {
            auto scatter = TargetProcess.CreateScatterHandle();
            if (scatter) {
                for (auto& w : work) {
                    if (!w.needsPose) continue;
                    auto& rig = rigCache[w.entityId];
                    TargetProcess.AddScatterReadRequest(scatter,
                        rig.layout.matrices,
                        poseBuf.data() + w.poseOffset, w.poseSize);
                }
                TargetProcess.ExecuteReadScatter(scatter);
                TargetProcess.CloseScatterHandle(scatter);
            }
        }

        // ---------------------------------------------------------------
        // Phase 5: Process poses, build entities, wall check.
        // ---------------------------------------------------------------
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
            if (detail.characterModel != w.characterModel ||
                now - detail.stamp > 500)
            {
                detail = DetailCacheEntry{};
                detail.characterModel = w.characterModel;
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
                detail.isDead = scratch.isDead;
                detail.isSprinting = scratch.isSprinting;
                detail.isCrouching = scratch.isCrouching;
                detail.role = scratch.role;
                detail.inVehicle = scratch.inVehicle;
                detail.vehicleId = scratch.vehicleId;

                const auto slotIt = slots.find(w.entityId);
                if (slotIt != slots.end()) {
                    ReadSlotIdentity(slotIt->second, scratch);
                    detail.nickname = std::move(scratch.nickname);
                    detail.clanTag = std::move(scratch.clanTag);
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
            entity.inVehicle = detail.inVehicle;
            entity.vehicleId = detail.vehicleId;
            entity.nickname = detail.nickname;
            entity.clanTag = detail.clanTag;

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
                    if (now - rig.poseFailureSince >= 750) {
                        rig.layoutValid = false;
                        rig.bones.clear();
                        rig.points = Skeleton::BonePoints{};
                        rig.poseFailures = 0;
                    }
                } else {
                    rig.poseFailures = 0;
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

                    if (moved)
                        rig.lastMotionTick = now;
                    rig.animatorActive = rig.lastMotionTick == 0 ||
                        now - rig.lastMotionTick < kFreezeMs;

                    for (auto& seg : rig.bones)
                        seg.visible = rig.animatorActive;

                    rig.prevLocal.swap(rig.curLocal);
                    rig.prevLocalValid = true;
                }
            } else if (!rig.layoutValid && now - rig.fallbackStamp > 1000) {
                rig.fallbackStamp = now;
                rig.bones = Skeleton::BuildSkeleton(cachedGame, cachedBattleModel,
                    entity.entityId, DmaRead);
                rig.points = Skeleton::BonePoints{};
            }
            entity.bones = rig.bones;
            entity.bonePoints = rig.points;
            entity.visible = rig.animatorActive;

            if (ConfigInstance.Player.WallCheck && g_bvhScene.valid &&
                cameraState.valid && entity.entityId != localEntityId)
            {
                const CollisionBVH::Float3 cam = {
                    cameraState.position.x,
                    cameraState.position.y,
                    cameraState.position.z };
                bool anyVisible = false;
                for (auto& seg : entity.bones) {
                    const CollisionBVH::Float3 mid = {
                        (seg.a.x + seg.b.x) * 0.5f,
                        (seg.a.y + seg.b.y) * 0.5f,
                        (seg.a.z + seg.b.z) * 0.5f };
                    if (CollisionBVH::IsOccluded(g_bvhScene, cam, mid))
                        seg.visible = false;
                    else
                        anyVisible = true;
                }
                if (!anyVisible)
                    entity.visible = false;
            }

            if (entity.entityId == localEntityId && localPosition.IsZero() &&
                !entity.position.IsZero()) {
                localPosition = entity.position;
            }

            entities.push_back(std::move(entity));
        }
    }

    const std::uint64_t passNow = GetTickCount64();

    if (cachedBattleModel > 0x10000 &&
        (worldRefreshStamp == 0 || passNow - worldRefreshStamp >= 250)) {
        auto state = std::make_shared<RobRunWorld::State>();
        if (RobRunWorld::Read(cachedBattleModel, cachedBattleEntities, *state)) {
            std::lock_guard<std::mutex> lock(g_worldMutex);
            g_worldState = std::move(state);
        }
        worldRefreshStamp = passNow;
    }

    if (cachedLocalCharacter > 0x10000 &&
        (inventoryRefreshStamp == 0 || passNow - inventoryRefreshStamp >= 1000)) {
        auto state = std::make_shared<RobRunInventory::State>();
        if (RobRunInventory::Read(cachedLocalCharacter, *state, readCatalog.load())) {
            std::lock_guard<std::mutex> lock(g_worldMutex);
            g_inventoryState = std::move(state);
        }
        inventoryRefreshStamp = passNow;
    }

    // A root change here means this pass rebuilt the scene identity. Bump the
    // generation so memory aim blocks writes while the new objects settle.
    NoteSessionState();

    PruneCaches();
    failureCount = 0;
}

void Update()
{
    if (!g_bvhScene.valid &&
        !ConfigInstance.Player.BvhPath.empty() &&
        ConfigInstance.Player.BvhPath != g_bvhLoadedPath)
    {
        g_bvhScene = CollisionBVH::Scene{};
        if (CollisionBVH::Load(ConfigInstance.Player.BvhPath, g_bvhScene))
            g_bvhLoadedPath = ConfigInstance.Player.BvhPath;
        else
            g_bvhLoadedPath.clear();
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
    // Managed camera first. Its position is never written by aim, so it is the
    // anchor used to identify which tagged Unity camera is the real render
    // camera; its orientation may be our aim angle, so the native camera is
    // only accepted when it is the same physical camera.
    RobRunCamera::CameraState managed{};
    const std::uintptr_t model = g_renderCameraModel.load(std::memory_order_acquire);
    if (model) {
        managed = RobRunCamera::Read(model);
        if (managed.valid) {
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
    return s;
}

} // namespace RobRunReader
