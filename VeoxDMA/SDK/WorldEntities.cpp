#include "pch.h"
#include "WorldEntities.h"
#include "RobRunSDK.h"
#include "RobRunMemory.h"
#include "Il2Cpp.h"
#include "Memory.h"
#include "DebugLog.h"
#include <cmath>
#include <functional>
#include <unordered_map>

namespace RobRunWorld {

void Init(std::uintptr_t gameAssemblyBase)
{
    Il2Cpp::Init(gameAssemblyBase);
}

const char* KindName(Kind kind)
{
    switch (kind) {
    case Kind::Container:       return "Container";
    case Kind::Loot:            return "Loot";
    case Kind::StaticLoot:      return "Static Loot";
    case Kind::RandomLoot:      return "Loot";
    case Kind::Vehicle:         return "Vehicle";
    case Kind::Car:             return "Car";
    case Kind::Helicopter:      return "Helicopter";
    case Kind::Grenade:         return "Grenade";
    case Kind::LootNode:        return "Loot Node";
    case Kind::SafeDoor:        return "Safe";
    case Kind::StorageBox:      return "Storage Box";
    case Kind::Shelving:        return "Shelving";
    case Kind::CollectionTable: return "Collection Table";
    case Kind::PlotContainer:   return "Plot Container";
    case Kind::DroppedBag:      return "Dropped Bag";
    case Kind::VehicleLoot:     return "Vehicle Loot";
    case Kind::Cargo:           return "Cargo";
    case Kind::PlotLoot:        return "Plot Loot";
    case Kind::FurnitureLoot:   return "Furniture Loot";
    case Kind::BuildingLoot:    return "Building Loot";
    case Kind::CashLootNode:           return "Cash Node";
    case Kind::SettlementCash:         return "Settlement Cash";
    case Kind::MoneyWorkbench:         return "Money Workbench";
    case Kind::SecurityAlarm:          return "Security Alarm";
    case Kind::LocationSecurityAlarm:  return "Security Alarm";
    case Kind::SecurityPanel:          return "Alarm Panel";
    case Kind::MetalDetector:          return "Metal Detector";
    case Kind::FirstAidCabinet:        return "First Aid";
    case Kind::CryptoWorkbench:        return "Crypto Bench";
    case Kind::VehicleTrunk:           return "Vehicle Trunk";
    case Kind::DrugWorkbench:          return "Drug Lab";
    case Kind::DefenseBlock:           return "Defense Block";
    }
    return "World";
}

namespace {

bool ValidPosition(const Vector3& position)
{
    return RobRunMemory::Finite(position) &&
        std::fabs(position.x) < 1.0e6f &&
        std::fabs(position.y) < 1.0e6f &&
        std::fabs(position.z) < 1.0e6f;
}

bool ReadScalarPosition(std::uintptr_t transform, Vector3& out)
{
    out = Vector3{};
    if (transform <= 0x10000) return false;

    const float x = RobRunMemory::ReadFloat(transform + rr::TransformModel::X);
    const float y = RobRunMemory::ReadFloat(transform + rr::TransformModel::Y);
    const float z = RobRunMemory::ReadFloat(transform + rr::TransformModel::Z);
    out = Vector3(x, y, z);
    if (ValidPosition(out) && (x != 0.f || y != 0.f || z != 0.f)) return true;

    // Some transform models store scalar primitive wrappers instead of inline
    // floats. Resolve each wrapper and validate the triplet.
    float wx = 0.f, wy = 0.f, wz = 0.f;
    const std::uintptr_t px = RobRunMemory::ReadPtr(transform + rr::TransformModel::X);
    const std::uintptr_t py = RobRunMemory::ReadPtr(transform + rr::TransformModel::Y);
    const std::uintptr_t pz = RobRunMemory::ReadPtr(transform + rr::TransformModel::Z);
    if (Il2Cpp::ReadPrimitiveFloat(px, wx) &&
        Il2Cpp::ReadPrimitiveFloat(py, wy) &&
        Il2Cpp::ReadPrimitiveFloat(pz, wz)) {
        out = Vector3(wx, wy, wz);
        return ValidPosition(out);
    }
    return false;
}

bool ReadVehiclePosition(std::uintptr_t transform, Vector3& out)
{
    out = Vector3{};
    if (transform <= 0x10000) return false;

    const std::uintptr_t wrapper = RobRunMemory::ReadPtr(
        transform + rr::VehicleTransformModel::Position);
    if (Il2Cpp::ReadPrimitiveVector3(wrapper, out) && ValidPosition(out))
        return true;

    // Fallback: inline Vector3 at the same offset.
    if (RobRunMemory::ReadVector3(transform + rr::VehicleTransformModel::Position, out) &&
        ValidPosition(out)) {
        return true;
    }
    return false;
}

using PositionReader = std::function<bool(std::uintptr_t model, Vector3& out)>;

// Inline Vector3 stored directly on the model.
bool ReadInlineField(std::uintptr_t model, std::uintptr_t offset, Vector3& out)
{
    out = Vector3{};
    if (model <= 0x10000) return false;
    if (!RobRunMemory::ReadVector3(model + offset, out)) return false;
    return ValidPosition(out) &&
        (out.x != 0.f || out.y != 0.f || out.z != 0.f);
}

std::string ReadIdentifiedId(std::uintptr_t obj)
{
    if (obj <= 0x10000) return {};
    const std::uintptr_t strObj = RobRunMemory::ReadPtr(obj + rr::IdentifiedBase::Id);
    if (strObj <= 0x10000) return {};
    std::wstring wide;
    if (!RobRunMemory::ReadString(strObj, wide, 128)) return {};
    return RobRunMemory::WideToUtf8(wide);
}

// Inline Vector3 stored on the model's description object.
bool ReadDescriptionPosition(std::uintptr_t model, std::uintptr_t descriptionOffset,
    std::uintptr_t positionOffset, Vector3& out)
{
    out = Vector3{};
    if (model <= 0x10000) return false;
    const std::uintptr_t description = RobRunMemory::ReadPtr(model + descriptionOffset);
    if (description <= 0x10000) return false;
    if (!RobRunMemory::ReadVector3(description + positionOffset, out)) return false;
    return ValidPosition(out) &&
        (out.x != 0.f || out.y != 0.f || out.z != 0.f);
}

// Cheap structural probe. Used to avoid mistaking a real Dictionary's buckets
// array for a wrapper object when a stable snapshot fails transiently.
bool HasDictionaryHeader(std::uintptr_t dictionary)
{
    if (dictionary <= 0x10000) return false;
    const std::uintptr_t entries = RobRunMemory::ReadPtr(dictionary + rr::Dict::entries);
    const std::int32_t count = RobRunMemory::ReadInt32(dictionary + rr::Dict::count);
    return entries > 0x10000 && count >= 0 && count <= 65536;
}

// Resolve a world collection field to a Dictionary. Most models expose the
// dictionary directly, but a few wrap it in a reference object; try the direct
// field first and then the common wrapper slots.
bool ReadCollection(std::uintptr_t world, std::uintptr_t offset,
    std::vector<RobRunMemory::DictEntry>& out)
{
    out.clear();
    if (world <= 0x10000) return false;

    const std::uintptr_t field = RobRunMemory::ReadPtr(world + offset);
    if (field <= 0x10000) return false;

    if (RobRunMemory::ReadDictInt(field, out, false)) return true;
    if (HasDictionaryHeader(field)) return false;

    const std::uintptr_t inner = RobRunMemory::ReadPtr(field + 0x10);
    if (inner > 0x10000 && inner != field &&
        RobRunMemory::ReadDictInt(inner, out, false))
        return true;

    const std::uintptr_t inner18 = RobRunMemory::ReadPtr(field + 0x18);
    if (inner18 > 0x10000 && inner18 != field && inner18 != inner &&
        RobRunMemory::ReadDictInt(inner18, out, false))
        return true;

    return false;
}

// Structural probe for a collection field. The field can hold the dictionary
// directly or wrap it in a reference object, so the common wrapper slots are
// tried without copying the dictionary contents.
bool CollectionLooksValid(std::uintptr_t world, std::uintptr_t offset)
{
    if (world <= 0x10000) return false;
    const std::uintptr_t field = RobRunMemory::ReadPtr(world + offset);
    if (field <= 0x10000) return false;

    if (RobRunMemory::LooksLikeDictionary(field)) return true;

    const std::uintptr_t inner = RobRunMemory::ReadPtr(field + 0x10);
    if (inner > 0x10000 && inner != field &&
        RobRunMemory::LooksLikeDictionary(inner))
        return true;

    const std::uintptr_t inner18 = RobRunMemory::ReadPtr(field + 0x18);
    return inner18 > 0x10000 && inner18 != field && inner18 != inner &&
        RobRunMemory::LooksLikeDictionary(inner18);
}

bool WorldLooksValid(std::uintptr_t world)
{
    if (world <= 0x10000) return false;

    // Probe collections that exist in every loaded scene. A wrong base pointer
    // normally fails the dictionary structure checks. BattleCharacters is
    // included because a fresh match may have no loot or vehicles yet.
    return CollectionLooksValid(world, rr::WorldEntitiesModel::BattleCharacters) ||
        CollectionLooksValid(world, rr::WorldEntitiesModel::Containers) ||
        CollectionLooksValid(world, rr::WorldEntitiesModel::Loots) ||
        CollectionLooksValid(world, rr::WorldEntitiesModel::Vehicles) ||
        CollectionLooksValid(world, rr::WorldEntitiesModel::Cars) ||
        CollectionLooksValid(world, rr::WorldEntitiesModel::SafeDoors) ||
        CollectionLooksValid(world, rr::WorldEntitiesModel::LootNodes) ||
        CollectionLooksValid(world, rr::WorldEntitiesModel::CashLootNodes) ||
        CollectionLooksValid(world, rr::WorldEntitiesModel::SettlementCash) ||
        CollectionLooksValid(world, rr::WorldEntitiesModel::SettlementCash);
}

void AddDictionary(std::uintptr_t world, std::uintptr_t dictionaryOffset, Kind kind,
    const PositionReader& readPosition, State& out, int& kindCounter)
{
    std::vector<RobRunMemory::DictEntry> entries;
    if (!ReadCollection(world, dictionaryOffset, entries)) return;

    for (const auto& entry : entries) {
        const std::uintptr_t model = entry.value;
        if (model <= 0x10000) continue;

        Vector3 position{};
        if (!readPosition(model, position) || !ValidPosition(position)) continue;

        Entity entity{};
        entity.id = entry.key;
        entity.kind = kind;
        entity.position = position;
        entity.label = KindName(kind);
        out.entities.push_back(std::move(entity));
        ++kindCounter;

        if (out.entities.size() >= 4096) return;
    }
}

// High-speed scatter reader for description-based world collections.
// Reduces hundreds of individual DMA round trips down to two batched transactions.
void AddDictionaryDescriptionScatter(std::uintptr_t world, std::uintptr_t dictionaryOffset, Kind kind,
    std::uintptr_t descriptionOffset, std::uintptr_t positionOffset, State& out, int& kindCounter)
{
    std::vector<RobRunMemory::DictEntry> entries;
    if (!ReadCollection(world, dictionaryOffset, entries) || entries.empty()) return;

    struct ItemWork {
        std::int32_t id = 0;
        std::uintptr_t model = 0;
        std::uintptr_t desc = 0;
        Vector3 position{};
    };
    std::vector<ItemWork> work;
    work.reserve(entries.size());
    for (const auto& e : entries) {
        if (e.value > 0x10000)
            work.push_back({ e.key, e.value, 0, {} });
    }
    if (work.empty()) return;

    // Phase 1: scatter read description pointers
    Memory::ScopedScatter sc1(TargetProcess, /*cached=*/true);
    if (sc1) {
        for (auto& item : work) {
            TargetProcess.AddScatterReadRequest(sc1, item.model + descriptionOffset, &item.desc, sizeof(std::uintptr_t));
        }
        TargetProcess.ExecuteScatterReadCached(sc1);
    }

    // Phase 2: scatter read positions from valid description pointers
    std::vector<std::size_t> validDesc;
    validDesc.reserve(work.size());
    for (std::size_t i = 0; i < work.size(); ++i) {
        if (work[i].desc > 0x10000)
            validDesc.push_back(i);
    }

    if (!validDesc.empty()) {
        Memory::ScopedScatter sc2(TargetProcess, /*cached=*/true);
        if (sc2) {
            for (auto idx : validDesc) {
                TargetProcess.AddScatterReadRequest(sc2, work[idx].desc + positionOffset, &work[idx].position, sizeof(Vector3));
            }
            TargetProcess.ExecuteScatterReadCached(sc2);
        }
    }

    for (auto idx : validDesc) {
        const auto& item = work[idx];
        if (ValidPosition(item.position) && (item.position.x != 0.f || item.position.y != 0.f || item.position.z != 0.f)) {
            Entity entity{};
            entity.id = item.id;
            entity.kind = kind;
            entity.position = item.position;
            std::string name = ReadIdentifiedId(item.desc);
            entity.label = name.empty() ? KindName(kind) : std::move(name);
            out.entities.push_back(std::move(entity));
            ++kindCounter;
            if (out.entities.size() >= 4096) break;
        }
    }
}

// High-speed scatter reader for inline-position collections (e.g. PlotLoot, FurnitureLoot).
// Executes in a single batched DMA transaction.
void AddDictionaryInlineScatter(std::uintptr_t world, std::uintptr_t dictionaryOffset, Kind kind,
    std::uintptr_t positionOffset, State& out, int& kindCounter)
{
    std::vector<RobRunMemory::DictEntry> entries;
    if (!ReadCollection(world, dictionaryOffset, entries) || entries.empty()) return;

    struct ItemWork {
        std::int32_t id = 0;
        std::uintptr_t model = 0;
        Vector3 position{};
    };
    std::vector<ItemWork> work;
    work.reserve(entries.size());
    for (const auto& e : entries) {
        if (e.value > 0x10000)
            work.push_back({ e.key, e.value, {} });
    }
    if (work.empty()) return;

    Memory::ScopedScatter sc(TargetProcess, /*cached=*/true);
    if (sc) {
        for (auto& item : work) {
            TargetProcess.AddScatterReadRequest(sc, item.model + positionOffset, &item.position, sizeof(Vector3));
        }
        TargetProcess.ExecuteScatterReadCached(sc);
    }

    for (const auto& item : work) {
        if (ValidPosition(item.position) && (item.position.x != 0.f || item.position.y != 0.f || item.position.z != 0.f)) {
            Entity entity{};
            entity.id = item.id;
            entity.kind = kind;
            entity.position = item.position;
            entity.label = KindName(kind);
            out.entities.push_back(std::move(entity));
            ++kindCounter;
            if (out.entities.size() >= 4096) break;
        }
    }
}

} // namespace

bool Read(std::uintptr_t battleModel, std::uintptr_t battleEntities, State& out)
{
    out = State{};
    if (battleModel <= 0x10000) return false;

    // Dump path: BattleModel+0x510 -> WorldEntitiesModel.
    std::uintptr_t world = 0;
    {
        const std::uintptr_t candidate = RobRunMemory::ReadPtr(
            battleModel + rr::BattleModel::WorldEntitiesModel);
        if (WorldLooksValid(candidate))
            world = candidate;
    }

    // Older sessions expose the same model at BattleEntitiesModel+0x60.
    if (!world && battleEntities > 0x10000) {
        const std::uintptr_t candidate = RobRunMemory::ReadPtr(
            battleEntities + rr::BattleEntitiesModel::WorldEntitiesModel);
        if (WorldLooksValid(candidate))
            world = candidate;
    }

    // Older BattleModel slot.
    if (!world) {
        const std::uintptr_t candidate = RobRunMemory::ReadPtr(
            battleModel + rr::BattleModel::WorldEntitiesModelAlt);
        if (WorldLooksValid(candidate))
            world = candidate;
    }
    if (world <= 0x10000) {
        static int failCount = 0;
        if ((++failCount % 50) == 1)
            DLOG("WorldRead: no valid world ptr (bm=0x%llx be=0x%llx) fails=%d",
                 (unsigned long long)battleModel, (unsigned long long)battleEntities, failCount);
        return false;
    }

    out.worldPtr = world;
    out.valid = true;

    // Read vehicles FIRST so they are never truncated by the 4096-entity cap
    // on dense maps with thousands of loot/container objects.
    AddDictionary(world, rr::WorldEntitiesModel::Cars, Kind::Car,
        [](std::uintptr_t model, Vector3& position) {
            const std::uintptr_t transform = RobRunMemory::ReadPtr(
                model + rr::CarModel::Transform);
            return ReadVehiclePosition(transform, position);
        }, out, out.vehicles);

    AddDictionary(world, rr::WorldEntitiesModel::Helicopters, Kind::Helicopter,
        [](std::uintptr_t model, Vector3& position) {
            const std::uintptr_t transform = RobRunMemory::ReadPtr(
                model + rr::HelicopterModel::Transform);
            return ReadVehiclePosition(transform, position);
        }, out, out.vehicles);

    // Also pick up any generic VehicleModel entries not already listed in Cars/Helicopters.
    {
        std::unordered_set<int> seenVehIds;
        seenVehIds.reserve(out.entities.size());
        for (const auto& e : out.entities) seenVehIds.insert(e.id);
        std::vector<RobRunMemory::DictEntry> vehEntries;
        if (ReadCollection(world, rr::WorldEntitiesModel::Vehicles, vehEntries)) {
            for (const auto& entry : vehEntries) {
                if (entry.value <= 0x10000 || seenVehIds.count(entry.key)) continue;
                const std::uintptr_t transform = RobRunMemory::ReadPtr(
                    entry.value + rr::VehicleModel::Transform);
                Vector3 position{};
                if (!ReadVehiclePosition(transform, position)) continue;
                Entity entity{};
                entity.id = entry.key;
                entity.kind = Kind::Vehicle;
                entity.position = position;
                entity.label = KindName(Kind::Vehicle);
                out.entities.push_back(std::move(entity));
                seenVehIds.insert(entry.key);
                ++out.vehicles;
            }
        }
    }

    const std::size_t containerStart = out.entities.size();
    AddDictionary(world, rr::WorldEntitiesModel::Containers, Kind::Container,
        [](std::uintptr_t model, Vector3& position) {
            const std::uintptr_t positionModel = RobRunMemory::ReadPtr(
                model + rr::ContainerModel::Position);
            if (RobRunMemory::ReadVector3(positionModel + 0x10, position))
                return true;
            const std::uintptr_t description = RobRunMemory::ReadPtr(
                model + rr::ContainerModel::Description);
            return RobRunMemory::ReadVector3(description + 0x30, position);
        }, out, out.containers);

    // Container empty flag is read after the position pass so the ESP can
    // label searched containers without re-walking the dictionary.
    std::vector<RobRunMemory::DictEntry> containerEntries;
    if (ReadCollection(world, rr::WorldEntitiesModel::Containers, containerEntries)) {
        std::unordered_map<std::int32_t, std::size_t> containerIndex;
        for (std::size_t i = containerStart; i < out.entities.size(); ++i)
            containerIndex[out.entities[i].id] = i;

        for (const auto& entry : containerEntries) {
            const auto it = containerIndex.find(entry.key);
            if (it == containerIndex.end()) continue;
            const std::uintptr_t wrapper = RobRunMemory::ReadPtr(
                entry.value + rr::ContainerModel::IsEmpty);
            bool empty = false;
            if (wrapper > 0x10000 && Il2Cpp::ReadPrimitiveBool(wrapper, empty)) {
                auto& entity = out.entities[it->second];
                entity.hasEmptyFlag = true;
                entity.empty = empty;
            }
        }
    }

    // 0x38 is droppedLootBags in the current dump. It used to be read here as
    // the generic "loots" dictionary; reading it a second time under
    // DroppedBag below would duplicate every bag and double the DMA traffic,
    // so the DroppedBag pass is the only reader for this offset.

    const std::size_t staticLootStart = out.entities.size();
    AddDictionaryDescriptionScatter(world, rr::WorldEntitiesModel::StaticLootObjects, Kind::StaticLoot,
        rr::StaticLootObjectModel::Description, 0x30, out, out.loots);

    const std::size_t randomLootStart = out.entities.size();
    AddDictionaryDescriptionScatter(world, rr::WorldEntitiesModel::RandomLootObjects, Kind::RandomLoot,
        rr::RandomLootObjectModel::Description, 0x24, out, out.loots);

    // Enrich static loot labels with item names
    // desc → delegate (Func) → target (closure) → item desc → Identified.Id
    {
        std::vector<RobRunMemory::DictEntry> entries;
        if (ReadCollection(world, rr::WorldEntitiesModel::StaticLootObjects, entries)) {
            std::unordered_map<std::int32_t, std::size_t> idx;
            for (std::size_t i = staticLootStart; i < randomLootStart; ++i)
                idx[out.entities[i].id] = i;
            for (const auto& e : entries) {
                auto it = idx.find(e.key);
                if (it == idx.end()) continue;
                const std::uintptr_t desc = RobRunMemory::ReadPtr(
                    e.value + rr::StaticLootObjectModel::Description);
                if (desc <= 0x10000) continue;
                const std::uintptr_t func = RobRunMemory::ReadPtr(
                    desc + rr::StaticLootObjectDescription::LootItemFunc);
                if (func <= 0x10000) continue;
                const std::uintptr_t closure = RobRunMemory::ReadPtr(func + 0x10);
                if (closure <= 0x10000) continue;
                const std::uintptr_t itemDesc = RobRunMemory::ReadPtr(closure + 0x10);
                std::string name = ReadIdentifiedId(itemDesc);
                if (!name.empty())
                    out.entities[it->second].label = std::move(name);
            }
        }
    }

    // Enrich random loot labels with item names
    // _item wrapper → item desc → Identified.Id
    {
        std::vector<RobRunMemory::DictEntry> entries;
        if (ReadCollection(world, rr::WorldEntitiesModel::RandomLootObjects, entries)) {
            std::unordered_map<std::int32_t, std::size_t> idx;
            for (std::size_t i = randomLootStart; i < out.entities.size(); ++i)
                idx[out.entities[i].id] = i;
            for (const auto& e : entries) {
                auto it = idx.find(e.key);
                if (it == idx.end()) continue;
                const std::uintptr_t wrapper = RobRunMemory::ReadPtr(
                    e.value + rr::RandomLootObjectModel::Item);
                if (wrapper <= 0x10000) continue;
                const std::uintptr_t itemDesc = RobRunMemory::ReadPtr(wrapper + 0x18);
                std::string name = ReadIdentifiedId(itemDesc);
                if (!name.empty())
                    out.entities[it->second].label = std::move(name);
            }
        }
    }

    AddDictionary(world, rr::WorldEntitiesModel::Grenades, Kind::Grenade,
        [](std::uintptr_t model, Vector3& position) {
            const std::uintptr_t transform = RobRunMemory::ReadPtr(
                model + rr::GrenadeModel::Transform);
            return RobRunMemory::ReadVector3(
                transform + rr::GrenadeTransformModel::Position, position);
        }, out, out.grenades);

    // --- New dump: containers, storage and safes (scatter-accelerated) -
    AddDictionaryDescriptionScatter(world, rr::WorldEntitiesModel::LootNodes, Kind::LootNode,
        rr::LootNodeModel::Description, rr::LootNodeModel::DescriptionPosition, out, out.containers);

    AddDictionaryDescriptionScatter(world, rr::WorldEntitiesModel::SafeDoors, Kind::SafeDoor,
        rr::SafeDoorModel::Description, rr::SafeDoorModel::DescriptionPosition, out, out.containers);

    AddDictionaryDescriptionScatter(world, rr::WorldEntitiesModel::StorageBoxes, Kind::StorageBox,
        rr::StorageBoxModel::DescriptionModel, rr::StorageBoxModel::DescriptionPosition, out, out.containers);

    AddDictionaryDescriptionScatter(world, rr::WorldEntitiesModel::Shelvings, Kind::Shelving,
        rr::ShelvingModel::DescriptionModel, rr::ShelvingModel::DescriptionPosition, out, out.containers);

    // CollectionTables
    {
        std::vector<RobRunMemory::DictEntry> entries;
        if (ReadCollection(world, rr::WorldEntitiesModel::CollectionTables, entries)) {
            for (const auto& entry : entries) {
                const std::uintptr_t model = entry.value;
                if (model <= 0x10000) continue;

                const std::uintptr_t descModel = RobRunMemory::ReadPtr(
                    model + rr::CollectionTableModel::DescriptionModel);
                const std::uintptr_t locDesc = descModel > 0x10000
                    ? RobRunMemory::ReadPtr(descModel + rr::CollectionTableModel::DescValue) : 0;
                if (locDesc <= 0x10000) continue;

                Vector3 position{};
                if (!RobRunMemory::ReadVector3(locDesc + rr::CollectionTableModel::Position, position) ||
                    !ValidPosition(position))
                    continue;

                const float yaw = RobRunMemory::ReadFloat(
                    locDesc + rr::CollectionTableModel::Orientation + 0x04);
                const bool hasYaw = std::isfinite(yaw) && yaw != 0.f;

                int cash = 0;
                const std::uintptr_t rewardModel = RobRunMemory::ReadPtr(
                    model + rr::CollectionTableModel::RewardModel);
                if (rewardModel > 0x10000) {
                    const std::uintptr_t rewardData = RobRunMemory::ReadPtr(rewardModel + 0x10);
                    if (rewardData > 0x10000) {
                        for (std::uintptr_t off : { (std::uintptr_t)0x18, (std::uintptr_t)0x20 }) {
                            const std::uintptr_t cashData = RobRunMemory::ReadPtr(rewardData + off);
                            if (cashData > 0x10000) {
                                int val = RobRunMemory::ReadInt32(cashData + rr::CashAmountData::Value);
                                if (val > 0 && val < 100000000) { cash = val; break; }
                            }
                        }
                    }
                }

                Entity entity{};
                entity.id = entry.key;
                entity.kind = Kind::CollectionTable;
                entity.position = position;
                entity.cashAmount = cash;
                entity.hasCashAmount = (cash > 0);
                entity.yaw = yaw;
                entity.hasYaw = hasYaw;
                entity.label = "Collection Table";
                out.entities.push_back(std::move(entity));
                ++out.containers;
                if (out.entities.size() >= 4096) break;
            }
        }
    }

    AddDictionaryDescriptionScatter(world, rr::WorldEntitiesModel::PlotContainers, Kind::PlotContainer,
        rr::PlotContainerModel::Description, rr::PlotContainerModel::DescriptionPosition, out, out.containers);

    // --- New dump: dropped bags, vehicle loot, cargo and loose loot ----
    const std::size_t droppedBagStart = out.entities.size();
    AddDictionary(world, rr::WorldEntitiesModel::DroppedLootBags, Kind::DroppedBag,
        [](std::uintptr_t model, Vector3& position) {
            const std::uintptr_t transform = RobRunMemory::ReadPtr(
                model + rr::DroppedBagModel::Transform);
            if (ReadScalarPosition(transform, position)) return true;
            return ReadInlineField(model, rr::DroppedBagModel::InteractPosition, position);
        }, out, out.loots);

    // Label dropped bags by role (Robber Bag / Cop Bag)
    {
        std::vector<RobRunMemory::DictEntry> entries;
        if (ReadCollection(world, rr::WorldEntitiesModel::DroppedLootBags, entries)) {
            std::unordered_map<std::int32_t, std::size_t> idx;
            for (std::size_t i = droppedBagStart; i < out.entities.size(); ++i)
                idx[out.entities[i].id] = i;
            for (const auto& e : entries) {
                auto it = idx.find(e.key);
                if (it == idx.end()) continue;
                const std::uintptr_t roleW = RobRunMemory::ReadPtr(
                    e.value + rr::DroppedBagModel::Role);
                if (roleW <= 0x10000) continue;
                const std::uintptr_t roleDesc = RobRunMemory::ReadPtr(roleW + 0x18);
                std::string roleName = ReadIdentifiedId(roleDesc);
                if (roleName.find("Police") != std::string::npos)
                    out.entities[it->second].label = "Cop Bag";
                else if (!roleName.empty())
                    out.entities[it->second].label = "Robber Bag";
            }
        }
    }

    const std::size_t vehicleLootStart = out.entities.size();
    AddDictionaryInlineScatter(world, rr::WorldEntitiesModel::VehicleLoots, Kind::VehicleLoot,
        rr::VehicleLootModel::InteractPosition, out, out.loots);

    // Flag vehicle loot that carries cargo
    {
        std::vector<RobRunMemory::DictEntry> entries;
        if (ReadCollection(world, rr::WorldEntitiesModel::VehicleLoots, entries)) {
            std::unordered_map<std::int32_t, std::size_t> idx;
            for (std::size_t i = vehicleLootStart; i < out.entities.size(); ++i)
                idx[out.entities[i].id] = i;
            for (const auto& e : entries) {
                auto it = idx.find(e.key);
                if (it == idx.end()) continue;
                const std::uintptr_t cargoW = RobRunMemory::ReadPtr(
                    e.value + rr::VehicleLootModel::HasCargo);
                bool hasCargo = false;
                if (cargoW > 0x10000 && Il2Cpp::ReadPrimitiveBool(cargoW, hasCargo) && hasCargo)
                    out.entities[it->second].label = "Vehicle Loot [C]";
            }
        }
    }

    AddDictionary(world, rr::WorldEntitiesModel::Cargos, Kind::Cargo,
        [](std::uintptr_t model, Vector3& position) {
            const std::uintptr_t transform = RobRunMemory::ReadPtr(
                model + rr::CargoModel::Transform);
            if (ReadVehiclePosition(transform, position)) return true;
            return ReadInlineField(model, rr::CargoModel::InteractPosition, position);
        }, out, out.loots);

    AddDictionaryInlineScatter(world, rr::WorldEntitiesModel::PlotLoots, Kind::PlotLoot,
        rr::PlotLootModel::Position, out, out.loots);

    AddDictionaryInlineScatter(world, rr::WorldEntitiesModel::FurnitureLoots, Kind::FurnitureLoot,
        rr::FurnitureLootModel::Position, out, out.loots);

    AddDictionaryInlineScatter(world, rr::WorldEntitiesModel::BuildingLoots, Kind::BuildingLoot,
        rr::BuildingLootModel::Position, out, out.loots);

    // --- Valuables with cash amounts ------------------------------------
    // CashLootNodes
    {
        std::vector<RobRunMemory::DictEntry> entries;
        if (ReadCollection(world, rr::WorldEntitiesModel::CashLootNodes, entries)) {
            for (const auto& entry : entries) {
                const std::uintptr_t model = entry.value;
                if (model <= 0x10000) continue;

                Vector3 position{};
                if (!ReadDescriptionPosition(model, rr::CashLootNodeModel::Description,
                    rr::CashLootNodeModel::DescriptionPosition, position))
                    continue;

                int cashAmount = 0;
                bool hasCash = false;
                const std::uintptr_t cashData = RobRunMemory::ReadPtr(
                    model + rr::CashLootNodeModel::CashAmountData);
                if (cashData > 0x10000) {
                    cashAmount = RobRunMemory::ReadInt32(cashData + rr::CashAmountData::Value);
                    hasCash = true;
                }

                const std::uintptr_t desc = RobRunMemory::ReadPtr(
                    model + rr::CashLootNodeModel::Description);
                std::string name = ReadIdentifiedId(desc);

                Entity entity{};
                entity.id = entry.key;
                entity.kind = Kind::CashLootNode;
                entity.position = position;
                entity.cashAmount = cashAmount;
                entity.hasCashAmount = hasCash;
                entity.label = name.empty() ? "Cash Node" : std::move(name);
                out.entities.push_back(std::move(entity));
                ++out.valuables;
                if (out.entities.size() >= 4096) break;
            }
        }
    }

    // SettlementCash — splits into cash/tax/conquest sub-entities
    {
        std::vector<RobRunMemory::DictEntry> entries;
        if (ReadCollection(world, rr::WorldEntitiesModel::SettlementCash, entries)) {
            for (const auto& entry : entries) {
                const std::uintptr_t model = entry.value;
                if (model <= 0x10000) continue;

                const std::uintptr_t desc = RobRunMemory::ReadPtr(
                    model + rr::SettlementCashModel::Description);
                if (desc <= 0x10000) continue;

                const std::int32_t cashId = RobRunMemory::ReadInt32(desc + rr::SettlementCashModel::EntityId);
                const std::int32_t taxId = RobRunMemory::ReadInt32(desc + rr::SettlementCashModel::TaxEntityId);

                const std::uintptr_t data = RobRunMemory::ReadPtr(
                    model + rr::SettlementCashModel::Data);

                Vector3 position{};
                std::string label;
                int cashAmount = 0;
                bool hasCash = false;

                if (entry.key == cashId) {
                    if (!RobRunMemory::ReadVector3(desc + rr::SettlementCashModel::CashPoint, position) ||
                        !ValidPosition(position))
                        continue;
                    label = "Settlement Cash";
                    if (data > 0x10000) {
                        const std::uintptr_t amountObj = RobRunMemory::ReadPtr(
                            data + rr::SettlementCashPublicData::CashAmount);
                        if (amountObj > 0x10000) {
                            cashAmount = RobRunMemory::ReadInt32(amountObj + rr::CashAmountData::Value);
                            hasCash = true;
                        }
                    }
                } else if (entry.key == taxId) {
                    if (!RobRunMemory::ReadVector3(desc + rr::SettlementCashModel::TaxPoint, position) ||
                        !ValidPosition(position))
                        continue;
                    label = "Settlement Tax";
                    if (data > 0x10000) {
                        const std::uintptr_t amountObj = RobRunMemory::ReadPtr(
                            data + rr::SettlementCashPublicData::TaxCashAmount);
                        if (amountObj > 0x10000) {
                            cashAmount = RobRunMemory::ReadInt32(amountObj + rr::CashAmountData::Value);
                            hasCash = true;
                        }
                    }
                } else {
                    if (!ReadDescriptionPosition(model, rr::SettlementCashModel::Description,
                        rr::SettlementCashModel::DescriptionPosition, position))
                        continue;
                    label = "Settlement Cash";
                }

                Entity entity{};
                entity.id = entry.key;
                entity.kind = Kind::SettlementCash;
                entity.position = position;
                entity.cashAmount = cashAmount;
                entity.hasCashAmount = hasCash;
                entity.label = std::move(label);
                out.entities.push_back(std::move(entity));
                ++out.valuables;
                if (out.entities.size() >= 4096) break;
            }
        }
    }

    // MoneyWorkbenches
    {
        std::vector<RobRunMemory::DictEntry> entries;
        if (ReadCollection(world, rr::WorldEntitiesModel::MoneyWorkbenches, entries)) {
            for (const auto& entry : entries) {
                const std::uintptr_t model = entry.value;
                if (model <= 0x10000) continue;

                const std::uintptr_t descModel = RobRunMemory::ReadPtr(
                    model + rr::MoneyWorkbenchModel::DescriptionModel);
                const std::uintptr_t locDesc = descModel > 0x10000
                    ? RobRunMemory::ReadPtr(descModel + rr::MoneyWorkbenchModel::DescValue) : 0;
                if (locDesc <= 0x10000) continue;

                Vector3 position{};
                if (!RobRunMemory::ReadVector3(locDesc + rr::MoneyWorkbenchModel::Position, position) ||
                    !ValidPosition(position))
                    continue;

                const float yaw = RobRunMemory::ReadFloat(
                    locDesc + rr::MoneyWorkbenchModel::Orientation + 0x04);
                const bool hasYaw = std::isfinite(yaw) && yaw != 0.f;

                int dirtyCash = 0;
                int cleanCash = 0;
                const std::uintptr_t data = RobRunMemory::ReadPtr(
                    model + rr::MoneyWorkbenchModel::Data);
                if (data > 0x10000) {
                    const std::uintptr_t cleanObj = RobRunMemory::ReadPtr(
                        data + rr::MoneyWorkbenchData::CleanAmount);
                    const std::uintptr_t dirtyObj = RobRunMemory::ReadPtr(
                        data + rr::MoneyWorkbenchData::DirtyAmount);
                    if (cleanObj > 0x10000)
                        cleanCash = RobRunMemory::ReadInt32(cleanObj + rr::CashAmountData::Value);
                    if (dirtyObj > 0x10000)
                        dirtyCash = RobRunMemory::ReadInt32(dirtyObj + rr::CashAmountData::Value);
                }

                Entity entity{};
                entity.id = entry.key;
                entity.kind = Kind::MoneyWorkbench;
                entity.position = position;
                entity.cashAmount = cleanCash + dirtyCash;
                entity.cleanCash = cleanCash;
                entity.dirtyCash = dirtyCash;
                entity.hasCashAmount = (entity.cashAmount > 0 || cleanCash > 0 || dirtyCash > 0);
                entity.yaw = yaw;
                entity.hasYaw = hasYaw;
                entity.label = "Money Table";
                out.entities.push_back(std::move(entity));
                ++out.valuables;
                if (out.entities.size() >= 4096) break;
            }
        }
    }

    // --- Security, First Aid, Crypto & Trunks -------------------------
    AddDictionary(world, rr::WorldEntitiesModel::SecurityAlarms, Kind::SecurityAlarm,
        [](std::uintptr_t model, Vector3& position) {
            return ReadDescriptionPosition(model, 0x28, 0x30, position) ||
                   ReadDescriptionPosition(model, 0x18, 0x10, position) ||
                   ReadInlineField(model, 0x10, position);
        }, out, out.alarms);

    AddDictionary(world, rr::WorldEntitiesModel::LocationSecurityAlarms, Kind::LocationSecurityAlarm,
        [](std::uintptr_t model, Vector3& position) {
            return ReadDescriptionPosition(model, 0x28, 0x30, position) ||
                   ReadDescriptionPosition(model, 0x18, 0x10, position) ||
                   ReadInlineField(model, 0x10, position);
        }, out, out.alarms);

    AddDictionary(world, rr::WorldEntitiesModel::SecurityPanels, Kind::SecurityPanel,
        [](std::uintptr_t model, Vector3& position) {
            const std::uintptr_t desc = RobRunMemory::ReadPtr(model + 0x18);
            if (desc > 0x10000)
                return RobRunMemory::ReadBytes(desc + 0x30, &position, sizeof(Vector3));
            return false;
        }, out, out.alarms);

    AddDictionary(world, rr::WorldEntitiesModel::MetalDetectors, Kind::MetalDetector,
        [](std::uintptr_t model, Vector3& position) {
            const std::uintptr_t desc = RobRunMemory::ReadPtr(model + 0x20);
            if (desc > 0x10000)
                return RobRunMemory::ReadBytes(desc + 0x30, &position, sizeof(Vector3));
            return false;
        }, out, out.alarms);

    AddDictionary(world, rr::WorldEntitiesModel::FirstAidCabinetModels, Kind::FirstAidCabinet,
        [](std::uintptr_t model, Vector3& position) {
            return ReadInlineField(model, 0x10, position) ||
                   ReadDescriptionPosition(model, 0x18, 0x10, position);
        }, out, out.firstAid);

    AddDictionary(world, rr::WorldEntitiesModel::CryptoWorkbenchesPublic, Kind::CryptoWorkbench,
        [](std::uintptr_t model, Vector3& position) {
            return ReadInlineField(model, 0x10, position) ||
                   ReadDescriptionPosition(model, 0x18, 0x10, position);
        }, out, out.workbenches);

    AddDictionary(world, rr::WorldEntitiesModel::VehicleTrunks, Kind::VehicleTrunk,
        [](std::uintptr_t model, Vector3& position) {
            return ReadInlineField(model, 0x10, position) ||
                   ReadDescriptionPosition(model, 0x18, 0x10, position);
        }, out, out.containers);

    AddDictionary(world, rr::WorldEntitiesModel::CargoChargeWorkbenches, Kind::DrugWorkbench,
        [](std::uintptr_t model, Vector3& position) {
            return ReadInlineField(model, 0x10, position) ||
                   ReadDescriptionPosition(model, 0x18, 0x10, position);
        }, out, out.drugWorkbenches);

    // --- Defense blocks (fuses) -------------------------------------------
    {
        std::vector<RobRunMemory::DictEntry> entries;
        if (ReadCollection(world, rr::WorldEntitiesModel::DefenseBlockPublicModels, entries)) {
            for (const auto& entry : entries) {
                const std::uintptr_t model = entry.value;
                if (model <= 0x10000) continue;

                // DescriptionModel<LocationDefenseBlockDescription> wrapper at +0x10
                const std::uintptr_t descWrapper = RobRunMemory::ReadPtr(
                    model + rr::DefenseBlockPublicModel::DescriptionModel);
                if (descWrapper <= 0x10000) continue;

                // DescriptionModel.Value at +0x10 (first field after IL2CPP header)
                const std::uintptr_t desc = RobRunMemory::ReadPtr(descWrapper + 0x10);
                if (desc <= 0x10000) continue;

                // LocationDefenseBlockDescription.Position at +0x28
                Vector3 position{};
                if (!RobRunMemory::ReadVector3(desc + 0x28, position) ||
                    !ValidPosition(position) ||
                    (position.x == 0.f && position.y == 0.f && position.z == 0.f))
                    continue;

                bool isRaided = false;
                bool isSlotsEmpty = false;
                const std::uintptr_t raidedWrapper = RobRunMemory::ReadPtr(
                    model + rr::DefenseBlockPublicModel::IsRaided);
                if (raidedWrapper > 0x10000)
                    Il2Cpp::ReadPrimitiveBool(raidedWrapper, isRaided);
                const std::uintptr_t emptyWrapper = RobRunMemory::ReadPtr(
                    model + rr::DefenseBlockPublicModel::IsSlotsEmpty);
                if (emptyWrapper > 0x10000)
                    Il2Cpp::ReadPrimitiveBool(emptyWrapper, isSlotsEmpty);

                Entity entity{};
                entity.id = entry.key;
                entity.kind = Kind::DefenseBlock;
                entity.position = position;
                entity.hasEmptyFlag = true;
                entity.empty = isSlotsEmpty;
                entity.label = isRaided ? "Fuse [raided]" : "Fuse";
                out.entities.push_back(std::move(entity));
                ++out.defenseBlocks;
                if (out.entities.size() >= 4096) break;
            }
        }
    }

    // --- Hidden players (cupboards / trash cans) ----------------------------
    {
        std::vector<RobRunMemory::DictEntry> entries;
        if (ReadCollection(world, rr::WorldEntitiesModel::HideObjects, entries)) {
            for (const auto& entry : entries) {
                if (entry.value <= 0x10000) continue;
                const int eid = RobRunMemory::ReadInt32(
                    entry.value + rr::HideObjectModel::EntityId);
                if (eid >= 0)
                    out.hiddenEntityIds.insert(eid);
            }
        }
    }

    static int readCount = 0;
    if ((++readCount % 50) == 1)
        DLOG("WorldRead #%d: world=0x%llx entities=%zu (cont=%d loot=%d veh=%d gren=%d)",
             readCount, (unsigned long long)world, out.entities.size(),
             out.containers, out.loots, out.vehicles, out.grenades);

    return out.valid;
}

// --- Plot / house reader --------------------------------------------------

static std::string ReadBuildingName(std::uintptr_t descModelPtr)
{
    if (descModelPtr <= 0x10000) return {};
    const std::uintptr_t descValue = RobRunMemory::ReadPtr(descModelPtr + rr::DescriptionModelT::Value);
    if (descValue <= 0x10000) return {};
    const std::uintptr_t strObj = RobRunMemory::ReadPtr(descValue + rr::IdentifiedBase::Id);
    if (strObj <= 0x10000) return {};
    std::wstring wide;
    if (!RobRunMemory::ReadString(strObj, wide, 128)) return {};
    return RobRunMemory::WideToUtf8(wide);
}

static void ReadDefenseBlock(std::uintptr_t defPtr, BuildingInfo& info)
{
    if (defPtr <= 0x10000) return;
    info.hasDefense = true;

    const std::uintptr_t raidedW = RobRunMemory::ReadPtr(defPtr + rr::DefenseBlockPublicModel::IsRaided);
    if (raidedW > 0x10000) Il2Cpp::ReadPrimitiveBool(raidedW, info.isRaided);

    const std::uintptr_t emptyW = RobRunMemory::ReadPtr(defPtr + rr::DefenseBlockPublicModel::IsSlotsEmpty);
    if (emptyW > 0x10000) Il2Cpp::ReadPrimitiveBool(emptyW, info.isFusesEmpty);

    const std::uintptr_t lockW = RobRunMemory::ReadPtr(defPtr + rr::DefenseBlockPublicModel::RaidLockTs);
    if (lockW > 0x10000) Il2Cpp::ReadPrimitiveInt64(lockW, info.raidLockTs);
}

static int CountDoors(std::uintptr_t doorsDict, int& openCount)
{
    openCount = 0;
    if (doorsDict <= 0x10000) return 0;

    std::vector<RobRunMemory::DictEntry> doorEntries;
    if (!RobRunMemory::ReadDictInt(doorsDict, doorEntries, false))
        return 0;

    int total = 0;
    for (const auto& de : doorEntries) {
        if (de.value <= 0x10000) continue;
        ++total;
        const std::uintptr_t doorModel = RobRunMemory::ReadPtr(
            de.value + 0x28); // BuilderDoorModel.DoorModel at 0x28
        if (doorModel <= 0x10000) continue;
        const std::uintptr_t stateModel = RobRunMemory::ReadPtr(
            doorModel + rr::DoorModelOffset::State);
        if (stateModel <= 0x10000) continue;
        const std::uintptr_t stateW = RobRunMemory::ReadPtr(
            stateModel + rr::DoorStateModelOffset::_state);
        if (stateW <= 0x10000) continue;
        // Index 2 = Closed, anything else = opened
        const std::uintptr_t descPtr = RobRunMemory::ReadPtr(stateW + 0x18);
        if (descPtr > 0x10000) {
            const int idx = RobRunMemory::ReadInt32(descPtr + rr::IdentifiedBase::Index);
            if (idx != 2) ++openCount;
        }
    }
    return total;
}

bool ReadPlots(std::uintptr_t battleEntities, PlotState& out)
{
    out = PlotState{};
    if (battleEntities <= 0x10000) return false;

    // BattleEntitiesModel + 0x58 → _plotsModels: Dictionary<int, PlotModel>
    const std::uintptr_t plotsDict = RobRunMemory::ReadPtr(battleEntities + 0x58);
    if (plotsDict <= 0x10000) return false;

    std::vector<RobRunMemory::DictEntry> plotEntries;
    if (!RobRunMemory::ReadDictInt(plotsDict, plotEntries, false) || plotEntries.empty())
        return false;

    for (const auto& pe : plotEntries) {
        const std::uintptr_t pm = pe.value;
        if (pm <= 0x10000) continue;

        PlotInfo plot{};
        plot.plotId = pe.key;

        // Position: PlotModel + 0x28 → PlotDescription → Position (inline V3 at +0x10)
        const std::uintptr_t plotDesc = RobRunMemory::ReadPtr(pm + rr::PlotModel::PlotDescription);
        if (plotDesc <= 0x10000) continue;
        if (!RobRunMemory::ReadVector3(plotDesc + rr::PlotDescription::Position, plot.position))
            continue;
        if (!ValidPosition(plot.position)) continue;

        // Slot index: PlotSlotDescription at PM + 0x20 → Index at +0x18
        const std::uintptr_t slotDesc = RobRunMemory::ReadPtr(pm + rr::PlotModel::PlotSlotDescription);
        if (slotDesc > 0x10000)
            plot.slotIndex = RobRunMemory::ReadInt32(slotDesc + rr::IdentifiedBase::Index);

        // Owner: PlotData at PM + 0x10 → OwnerPublicId wrapper at +0x28
        const std::uintptr_t plotData = RobRunMemory::ReadPtr(pm + rr::PlotModel::_data);
        std::uint64_t ownerPubId = 0;
        if (plotData > 0x10000) {
            const std::uintptr_t pubIdW = RobRunMemory::ReadPtr(plotData + rr::PlotData::OwnerPublicId);
            if (pubIdW > 0x10000)
                Il2Cpp::ReadPrimitiveInt64(pubIdW, reinterpret_cast<std::int64_t&>(ownerPubId));
        }
        if (ownerPubId > 0)
            plot.ownerName = std::to_string(ownerPubId);

        // --- Main building ---
        const std::uintptr_t mainBld = RobRunMemory::ReadPtr(pm + rr::PlotModel::MainBuilding);
        if (mainBld > 0x10000) {
            plot.mainBuilding.name = ReadBuildingName(
                RobRunMemory::ReadPtr(mainBld + rr::MainBuildingModel::DescriptionModel));
            if (plot.mainBuilding.name.empty()) plot.mainBuilding.name = "House";

            ReadDefenseBlock(
                RobRunMemory::ReadPtr(mainBld + rr::MainBuildingModel::DefenseBlockPublicModel),
                plot.mainBuilding);

            const std::uintptr_t doorsD = RobRunMemory::ReadPtr(mainBld + rr::MainBuildingModel::Doors);
            plot.mainBuilding.doorCount = CountDoors(doorsD, plot.mainBuilding.doorsOpen);
        }

        // --- Compound wall ---
        const std::uintptr_t wallBld = RobRunMemory::ReadPtr(pm + rr::PlotModel::CompoundWall);
        if (wallBld > 0x10000) {
            plot.compoundWall.name = ReadBuildingName(
                RobRunMemory::ReadPtr(wallBld + rr::CompoundWallModel::DescriptionModel));
            if (plot.compoundWall.name.empty()) plot.compoundWall.name = "Wall";

            ReadDefenseBlock(
                RobRunMemory::ReadPtr(wallBld + rr::CompoundWallModel::DefenseBlockPublicModel),
                plot.compoundWall);

            const std::uintptr_t doorsD = RobRunMemory::ReadPtr(wallBld + rr::CompoundWallModel::Doors);
            plot.compoundWall.doorCount = CountDoors(doorsD, plot.compoundWall.doorsOpen);
        }

        // --- Outbuildings ---
        const std::uintptr_t outDict = RobRunMemory::ReadPtr(pm + rr::PlotModel::Outbuildings);
        if (outDict > 0x10000) {
            std::vector<RobRunMemory::DictEntry> outEntries;
            if (RobRunMemory::ReadDictInt(outDict, outEntries, false)) {
                for (const auto& oe : outEntries) {
                    if (oe.value <= 0x10000) continue;
                    BuildingInfo ob{};
                    ob.name = ReadBuildingName(
                        RobRunMemory::ReadPtr(oe.value + rr::OutbuildingModel::DescriptionModel));
                    if (ob.name.empty()) ob.name = "Outbuilding";

                    ReadDefenseBlock(
                        RobRunMemory::ReadPtr(oe.value + rr::OutbuildingModel::DefenseBlockPublicModel),
                        ob);

                    const std::uintptr_t doorsD = RobRunMemory::ReadPtr(
                        oe.value + rr::OutbuildingModel::Doors);
                    ob.doorCount = CountDoors(doorsD, ob.doorsOpen);

                    plot.outbuildings.push_back(std::move(ob));
                    if (plot.outbuildings.size() >= 16) break;
                }
            }
        }

        out.plots.push_back(std::move(plot));
        if (out.plots.size() >= 256) break;
    }

    out.valid = !out.plots.empty();
    return out.valid;
}

std::vector<ProbeResult> ProbeMoneyModels(std::uintptr_t worldPtr)
{
    std::vector<ProbeResult> results;
    if (worldPtr <= 0x10000) return results;

    struct { std::uintptr_t offset; const char* name; } dicts[] = {
        { rr::WorldEntitiesModel::CashLootNodes,    "CashLootNode" },
        { rr::WorldEntitiesModel::SettlementCash,   "SettlementCash" },
        { rr::WorldEntitiesModel::MoneyWorkbenches, "MoneyWorkbench" },
        { rr::WorldEntitiesModel::CollectionTables, "CollectionTable" },
    };

    for (auto& d : dicts) {
        std::vector<RobRunMemory::DictEntry> entries;
        if (!ReadCollection(worldPtr, d.offset, entries) || entries.empty())
            continue;

        const std::uintptr_t model = entries[0].value;
        if (model <= 0x10000) continue;

        ProbeResult r;
        r.dictName = d.name;
        r.ptr = model;

        const std::uintptr_t klass = Il2Cpp::ClassOf(model);
        if (klass > 0x10000) {
            auto info = Il2Cpp::GetClassInfo(klass);
            r.className = info.ns.empty() ? info.name : info.ns + "::" + info.name;

            for (const auto& field : Il2Cpp::Fields(klass)) {
                ProbeField pf;
                pf.name = field.name;
                pf.offset = field.offset;
                pf.typeCode = field.typeCode;
                pf.rawValue = 0;
                RobRunMemory::ReadBytes(model + field.offset, &pf.rawValue, 8);
                r.fields.push_back(std::move(pf));
            }
        }
        results.push_back(std::move(r));
    }
    return results;
}

} // namespace RobRunWorld
