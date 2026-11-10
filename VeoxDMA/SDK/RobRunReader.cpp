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

// Worker-published snapshot. Only the worker writes; any thread may read.
static std::mutex g_snapshotMutex;
static std::shared_ptr<const Snapshot> g_snapshot = std::make_shared<Snapshot>();
static std::atomic<bool> g_workerStop{ false };
static std::thread g_workerThread;

static bool DmaRead(std::uint64_t address, void* buffer, std::size_t size)
{
    return TargetProcess.Read(static_cast<std::uintptr_t>(address), buffer, size);
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
    worldState = RobRunWorld::State{};
    inventoryState = RobRunInventory::State{};
    cameraState = RobRunCamera::CameraState{};
    localEntityId = -1;
    localPosition = Vector3();
    fallbackViewProjection = Matrix4x4{};
    fallbackViewProjectionValid = false;

    {
        std::lock_guard<std::mutex> lock(g_snapshotMutex);
        g_snapshot = std::make_shared<Snapshot>();
    }

    g_workerStop.store(false, std::memory_order_relaxed);
    g_workerThread = std::thread([] {
        while (!g_workerStop.load(std::memory_order_relaxed)) {
            Update();
            // ~30 Hz target, but a slow DMA pass simply runs slower instead of
            // blocking anything on the render thread.
            for (int i = 0; i < 32 && !g_workerStop.load(std::memory_order_relaxed); ++i)
                Sleep(1);
        }
    });
}

static std::uintptr_t ReadPtr(std::uintptr_t address)
{
    return RobRunMemory::ReadPtr(address);
}

static bool ReadVector3(std::uintptr_t address, Vector3& out)
{
    return RobRunMemory::ReadVector3(address, out);
}

static bool IsClass(std::uintptr_t object, const char* name)
{
    return Il2Cpp::IsClass(object, name);
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

// Skeleton extraction is far more expensive than the scalar entity reads, so
// it is refreshed at a lower rate and cached per entity between passes.
struct BoneCacheEntry {
    std::uintptr_t view = 0;
    std::uint64_t stamp = 0;
    std::vector<Skeleton::BoneSegment> bones;
};
static std::unordered_map<std::int32_t, BoneCacheEntry> boneCache;

static void UpdateInternal()
{
    entities.clear();
    worldState = RobRunWorld::State{};
    inventoryState = RobRunInventory::State{};
    localEntityId = -1;
    localPosition = Vector3{};
    fallbackViewProjection = Matrix4x4{};
    fallbackViewProjectionValid = false;

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
            ReadVector3(transform + rr::CharacterTransformModel::Position, localPosition);
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
    const std::uintptr_t slotsRoot = ReadPtr(
        cachedBattleEntities + rr::BattleEntitiesModel::CharacterSlotsRoot);
    if (slotsRoot > 0x10000) {
        const std::uintptr_t slotDictionary = ReadPtr(
            slotsRoot + rr::CharacterSlotsRoot::SlotDict);
        std::vector<RobRunMemory::DictEntry> slotEntries;
        if (RobRunMemory::ReadDictInt(slotDictionary, slotEntries)) {
            slots.reserve(slotEntries.size());
            for (const auto& entry : slotEntries)
                slots[entry.key] = entry.value;
        }
    }

    const std::uintptr_t entityDictionary = ReadPtr(
        cachedBattleEntities + rr::BattleEntitiesModel::EntityDict);
    std::vector<RobRunMemory::DictEntry> entityEntries;
    if (RobRunMemory::ReadDictInt(entityDictionary, entityEntries)) {
        entities.reserve(entityEntries.size());

        for (const auto& entry : entityEntries) {
            const std::uintptr_t entityModel = entry.value;
            if (entityModel <= 0x10000) continue;

            std::uintptr_t characterModel = ReadPtr(
                entityModel + rr::BattleCharacterEntityModel::BattleCharacterModel);
            if (characterModel <= 0x10000 && IsClass(entityModel, "BattleCharacterModel"))
                characterModel = entityModel;
            if (characterModel <= 0x10000) continue;

            const std::uintptr_t transform = ReadPtr(
                characterModel + rr::BattleCharacterModel::CharacterTransformModel);
            Vector3 position{};
            if (!ReadVector3(transform + rr::CharacterTransformModel::Position, position))
                continue;

            EntityData entity{};
            entity.entityId = entry.key;
            entity.position = position;
            entity.characterModel = characterModel;

            const float pitch = RobRunMemory::ReadFloat(
                transform + rr::CharacterTransformModel::Position + 0x0C);
            const float yaw = RobRunMemory::ReadFloat(
                transform + rr::CharacterTransformModel::Position + 0x10);
            if (std::isfinite(pitch)) entity.pitch = pitch;
            if (std::isfinite(yaw)) {
                entity.yaw = yaw;
                const float cosPitch = std::cos(pitch);
                entity.lookDirection = Vector3(
                    std::sin(yaw) * cosPitch,
                    std::sin(pitch),
                    std::cos(yaw) * cosPitch);
            }

            const std::uintptr_t deadWrapper = ReadPtr(
                characterModel + rr::BattleCharacterModel::IsDeadWrapper);
            bool dead = false;
            if (Il2Cpp::ReadPrimitiveBool(deadWrapper, dead))
                entity.isDead = dead;

            ReadHealth(characterModel, entity);
            ReadRoleAndVehicle(characterModel, entity);

            const auto viewIt = views.find(entry.key);
            if (viewIt != views.end())
                entity.viewPtr = viewIt->second;

            const auto slotIt = slots.find(entry.key);
            if (slotIt != slots.end())
                ReadSlotIdentity(slotIt->second, entity);

            // Refresh bones only when the view changed or the cache is stale.
            // The render thread reads these cached segments directly.
            const std::uint64_t now = GetTickCount64();
            auto& cachedBones = boneCache[entity.entityId];
            if (cachedBones.bones.empty() || cachedBones.view != entity.viewPtr ||
                now - cachedBones.stamp > 120) {
                std::vector<Skeleton::BoneSegment> bones;
                if (entity.viewPtr > 0x10000)
                    bones = Skeleton::BuildFromView(entity.viewPtr, DmaRead);
                if (bones.empty() && cachedGame > 0x10000 && cachedBattleModel > 0x10000)
                    bones = Skeleton::BuildSkeleton(cachedGame, cachedBattleModel,
                        entity.entityId, DmaRead);
                cachedBones.view = entity.viewPtr;
                cachedBones.stamp = now;
                cachedBones.bones = std::move(bones);
            }
            entity.bones = cachedBones.bones;

            if (entity.entityId == localEntityId && localPosition.IsZero() &&
                !position.IsZero()) {
                localPosition = position;
            }

            entities.push_back(std::move(entity));
        }
    }

    if (cachedBattleModel > 0x10000)
        RobRunWorld::Read(cachedBattleModel, worldState);

    if (cachedLocalCharacter > 0x10000)
        RobRunInventory::Read(cachedLocalCharacter, inventoryState, readCatalog.load());

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

bool GetViewProjection(Matrix4x4& out, int screenWidth, int screenHeight)
{
    if (screenWidth <= 0 || screenHeight <= 0) return false;
    const auto snapshot = GetSnapshot();
    if (!snapshot) return false;
    const float aspect = static_cast<float>(screenWidth) / static_cast<float>(screenHeight);
    return RobRunCamera::BuildViewProjection(snapshot->camera, aspect, out);
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
