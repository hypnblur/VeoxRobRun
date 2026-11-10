#include "pch.h"
#include "RobRunReader.h"
#include "RobRunMemory.h"
#include "Il2Cpp.h"
#include "Memory.h"
#include "CameraFinder.h"
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
static std::uintptr_t cachedUpdater = 0;

static std::vector<EntityData> entities;
static RobRunWorld::State worldState;
static RobRunInventory::State inventoryState;
static std::uint64_t worldRefreshStamp = 0;
static std::uint64_t inventoryRefreshStamp = 0;
static RobRunCamera::CameraState cameraState;
static int32_t localEntityId = -1;
static Vector3 localPosition{};
static std::uint32_t failureCount = 0;
static std::atomic<bool> readCatalog{ false };
static int updateCounter = 0;
static int gameRetryCountdown = 0;
static int updaterRetryCountdown = 0;

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
    std::vector<Skeleton::Vec> curLocal;
    std::vector<Skeleton::Vec> prevLocal;
    bool prevLocalValid = false;
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
    snapshot->world = worldState;
    snapshot->inventory = inventoryState;
    snapshot->camera = cameraState;
    snapshot->localEntityId = localEntityId;
    snapshot->localPosition = localPosition;
    snapshot->game = cachedGame;
    snapshot->battleModel = cachedBattleModel;
    snapshot->localCharacter = cachedLocalCharacter;
    snapshot->cameraModel = cachedCameraModel;
    snapshot->failureCount = failureCount;
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
    cachedUpdater = 0;
    failureCount = 0;
    updateCounter = 0;
    gameRetryCountdown = 0;
    updaterRetryCountdown = 0;
    entities.clear();
    detailCache.clear();
    rigCache.clear();
    worldState = RobRunWorld::State{};
    inventoryState = RobRunInventory::State{};
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

static void ReadSlotIdentity(std::uintptr_t slotModel, EntityData& entity)
{
    if (slotModel <= 0x10000) return;
    const std::uintptr_t slotData = ReadPtr(slotModel + rr::BattleCharacterSlotModel::_data);
    if (slotData <= 0x10000) return;

    const std::uintptr_t nickname = ReadPtr(slotData + rr::BattleCharacterSlotData::Nickname);
    RobRunMemory::ReadWrapperString(nickname, entity.nickname);

    const std::uintptr_t clan = ReadPtr(slotData + rr::BattleCharacterSlotData::ClanTag);
    RobRunMemory::ReadWrapperString(clan, entity.clanTag);
}

static void RevalidateSession()
{
    if (!gaBase) return;

    // The PE fingerprint is the hard gate. Accessor signatures are advisory:
    // they surface unexpected toolchain changes without forcing repeated
    // re-resolution when a signature prefix differs.
    if (!Il2Cpp::VerifyFingerprint()) {
        ++failureCount;
        cachedGame = 0;
        cachedBattleModel = 0;
        cachedBattleEntities = 0;
        cachedCameraModel = 0;
        cachedLocalCharacter = 0;
        cachedUpdater = 0;
        return;
    }
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

    if (!gaBase) return;

    const std::uint64_t passNow = GetTickCount64();

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
        worldState = RobRunWorld::State{};
        worldRefreshStamp = 0;
        inventoryState = RobRunInventory::State{};
        inventoryRefreshStamp = 0;
        ++failureCount;
        return;
    }

    if (!cachedBattleEntities)
        cachedBattleEntities = ReadPtr(cachedBattleModel + rr::BattleModel::BattleEntitiesModel);
    if (!IsClass(cachedBattleEntities, "BattleEntitiesModel")) {
        ++failureCount;
        return;
    }

    if (!cachedCameraModel)
        cachedCameraModel = ReadPtr(cachedBattleModel + rr::BattleModel::BattleCameraModel);
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
        }
    } else {
        cachedLocalCharacter = 0;
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

        for (const auto& entry : entityEntries) {
            const std::uintptr_t entityModel = entry.value;
            if (entityModel <= 0x10000) continue;

            // New offset set: +0x20 is the client model, which is the view the
            // player actually sees. Fall back to the simulated model and then
            // to the entity itself when the client pointer is not usable.
            std::uintptr_t characterModel = ReadPtr(
                entityModel + rr::BattleCharacterEntityModel::ClientModel);
            if (!IsClass(characterModel, "BattleCharacterModel")) {
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

            // Position (x/y/z), pitch and yaw are contiguous. One uncached
            // read per entity keeps the box on the character instead of
            // returning a VMM-cached sample that can trail by hundreds of ms.
            struct TransformBlock {
                Vector3 position;
                float pitch;
                float yaw;
            };
            static_assert(sizeof(TransformBlock) == 20, "transform block layout");
            TransformBlock block{};
            const std::uintptr_t blockAddress =
                transform + rr::CharacterTransformModel::Position;
            if ((!RobRunMemory::ReadBytesFresh(blockAddress, &block, sizeof(block)) &&
                 !RobRunMemory::ReadBytes(blockAddress, &block, sizeof(block))) ||
                !RobRunMemory::Finite(block.position)) {
                continue;
            }

            EntityData entity{};
            entity.entityId = entry.key;
            entity.position = block.position;
            entity.characterModel = characterModel;
            if (std::isfinite(block.pitch)) entity.pitch = block.pitch;
            if (std::isfinite(block.yaw)) {
                entity.yaw = block.yaw;
                const float cosPitch = std::cos(block.pitch);
                entity.lookDirection = Vector3(
                    std::sin(block.yaw) * cosPitch,
                    std::sin(block.pitch),
                    std::cos(block.yaw) * cosPitch);
            }

            const std::uint64_t now = GetTickCount64();

            // Identity, health, role and vehicle state change slowly and are
            // expensive to read (primitive wrappers + strings). Refresh them a
            // few times per second and reuse the values in between.
            auto& detail = detailCache[entity.entityId];
            if (detail.characterModel != characterModel || now - detail.stamp > 500) {
                detail = DetailCacheEntry{};
                detail.characterModel = characterModel;
                detail.stamp = now;

                EntityData scratch{};
                scratch.entityId = entity.entityId;

                const std::uintptr_t deadWrapper = ReadPtr(
                    characterModel + rr::BattleCharacterModel::IsDeadWrapper);
                bool dead = false;
                if (Il2Cpp::ReadPrimitiveBool(deadWrapper, dead))
                    detail.isDead = dead;

                ReadHealth(characterModel, scratch);
                detail.health = scratch.health;
                detail.maxHealth = scratch.maxHealth;
                detail.criticalHealth = scratch.criticalHealth;
                detail.healthKnown = scratch.healthKnown;

                // Keep the wrapper-derived death flag as the default; the
                // CommonData read below may override it when available.
                scratch.isDead = detail.isDead;
                ReadRoleAndVehicle(characterModel, scratch);
                ReadCommonState(characterModel, scratch);
                detail.isDead = scratch.isDead;
                detail.isSprinting = scratch.isSprinting;
                detail.isCrouching = scratch.isCrouching;
                detail.role = scratch.role;
                detail.inVehicle = scratch.inVehicle;
                detail.vehicleId = scratch.vehicleId;

                const auto slotIt = slots.find(entry.key);
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

            const auto viewIt = views.find(entry.key);
            if (viewIt != views.end())
                entity.viewPtr = viewIt->second;

            // Resolve the rig layout once per view, then sample the animation
            // pose every pass. Sampling is a single read of the matrix array,
            // so bones stay in sync with the position instead of being served
            // from a 100+ ms cache.
            auto& rig = rigCache[entry.key];
            if (rig.view != entity.viewPtr) {
                rig.view = entity.viewPtr;
                rig.layoutValid = false;
                rig.layoutStamp = 0;
                rig.bones.clear();
                rig.points = Skeleton::BonePoints{};
                rig.prevLocalValid = false;
            }

            const bool dueForRebuild = !rig.layoutValid
                ? (rig.layoutStamp == 0 || now - rig.layoutStamp >= 250)
                : (now - rig.layoutStamp >= 2000);
            if (dueForRebuild) {
                rig.layoutStamp = now;
                rig.layoutValid = entity.viewPtr > 0x10000 &&
                    Skeleton::BuildLayoutFromView(entity.viewPtr, DmaRead, rig.layout);
                if (!rig.layoutValid) {
                    rig.bones.clear();
                    rig.points = Skeleton::BonePoints{};
                }
            }

            if (rig.layoutValid) {
                rig.curLocal.resize(static_cast<std::size_t>(rig.layout.count));
                if (!Skeleton::SampleLayout(rig.layout, DmaReadFresh, rig.bones, &rig.points, rig.curLocal.data()) &&
                    !Skeleton::SampleLayout(rig.layout, DmaRead, rig.bones, &rig.points, rig.curLocal.data())) {
                    rig.layoutValid = false;
                } else {
                    constexpr float kStaleThreshSq = 0.0001f * 0.0001f;
                    if (rig.prevLocalValid && rig.prevLocal.size() == rig.curLocal.size()) {
                        for (std::size_t i = 0; i < rig.bones.size() && i < rig.layout.segments.size(); ++i) {
                            const int pi = rig.layout.segments[i].first;
                            const int ci = rig.layout.segments[i].second;
                            auto distSq = [&](int idx) {
                                float dx = rig.curLocal[idx].x - rig.prevLocal[idx].x;
                                float dy = rig.curLocal[idx].y - rig.prevLocal[idx].y;
                                float dz = rig.curLocal[idx].z - rig.prevLocal[idx].z;
                                return dx*dx + dy*dy + dz*dz;
                            };
                            rig.bones[i].visible = distSq(pi) > kStaleThreshSq || distSq(ci) > kStaleThreshSq;
                        }
                    }
                    rig.prevLocal.swap(rig.curLocal);
                    rig.prevLocalValid = true;
                }
            } else if (now - rig.fallbackStamp > 1000) {
                // Last resort when the view layout cannot be validated: full
                // extraction, at most once per second.
                rig.fallbackStamp = now;
                rig.bones = Skeleton::BuildSkeleton(cachedGame, cachedBattleModel,
                    entity.entityId, DmaRead);
                rig.points = Skeleton::BonePoints{};
            }
            entity.bones = rig.bones;
            entity.bonePoints = rig.points;

            if (entity.entityId == localEntityId && localPosition.IsZero() &&
                !entity.position.IsZero()) {
                localPosition = entity.position;
            }

            entities.push_back(std::move(entity));
        }
    }

    // World markers and inventory are expensive (many dictionaries and
    // per-item reads) and change slowly. Refresh them on their own cadence so
    // the player pass stays short and positions/bones stay fresh.
    if (cachedBattleModel > 0x10000 &&
        (worldRefreshStamp == 0 || passNow - worldRefreshStamp >= 250)) {
        RobRunWorld::Read(cachedBattleModel, cachedBattleEntities, worldState);
        worldRefreshStamp = passNow;
    }

    if (cachedLocalCharacter > 0x10000 &&
        (inventoryRefreshStamp == 0 || passNow - inventoryRefreshStamp >= 1000)) {
        RobRunInventory::Read(cachedLocalCharacter, inventoryState, readCatalog.load());
        inventoryRefreshStamp = passNow;
    }

    PruneCaches();
    failureCount = 0;
}

void Update()
{
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
    const std::uintptr_t model = g_renderCameraModel.load(std::memory_order_acquire);
    if (!model) return;
    auto camera = RobRunCamera::Read(model);
    if (!camera.valid) return;
    std::lock_guard<std::mutex> lock(g_freshCameraMutex);
    g_freshCamera = camera;
}

bool GetViewProjection(Matrix4x4& out, int screenWidth, int screenHeight)
{
    if (screenWidth <= 0 || screenHeight <= 0) return false;

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

} // namespace RobRunReader
