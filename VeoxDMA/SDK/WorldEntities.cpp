#include "pch.h"
#include "WorldEntities.h"
#include "RobRunSDK.h"
#include "RobRunMemory.h"
#include "Il2Cpp.h"
#include "Memory.h"
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
    case Kind::CashLootNode:    return "Cash Node";
    case Kind::SettlementCash:  return "Settlement Cash";
    case Kind::MoneyWorkbench:  return "Money Workbench";
    case Kind::PointOfInterest: return "Point of Interest";
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
        CollectionLooksValid(world, rr::WorldEntitiesModel::PointOfInterest);
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
    if (world <= 0x10000) return false;

    out.worldPtr = world;
    out.valid = true;

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

    AddDictionary(world, rr::WorldEntitiesModel::StaticLootObjects, Kind::StaticLoot,
        [](std::uintptr_t model, Vector3& position) {
            const std::uintptr_t description = RobRunMemory::ReadPtr(
                model + rr::StaticLootObjectModel::Description);
            return RobRunMemory::ReadVector3(description + 0x30, position);
        }, out, out.loots);

    AddDictionary(world, rr::WorldEntitiesModel::RandomLootObjects, Kind::RandomLoot,
        [](std::uintptr_t model, Vector3& position) {
            const std::uintptr_t description = RobRunMemory::ReadPtr(
                model + rr::RandomLootObjectModel::Description);
            return RobRunMemory::ReadVector3(description + 0x24, position);
        }, out, out.loots);

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

    AddDictionary(world, rr::WorldEntitiesModel::Grenades, Kind::Grenade,
        [](std::uintptr_t model, Vector3& position) {
            const std::uintptr_t transform = RobRunMemory::ReadPtr(
                model + rr::GrenadeModel::Transform);
            return RobRunMemory::ReadVector3(
                transform + rr::GrenadeTransformModel::Position, position);
        }, out, out.grenades);

    // --- New dump: containers, storage and safes -----------------------
    AddDictionary(world, rr::WorldEntitiesModel::LootNodes, Kind::LootNode,
        [](std::uintptr_t model, Vector3& position) {
            return ReadDescriptionPosition(model, rr::LootNodeModel::Description,
                rr::LootNodeModel::DescriptionPosition, position);
        }, out, out.containers);

    AddDictionary(world, rr::WorldEntitiesModel::SafeDoors, Kind::SafeDoor,
        [](std::uintptr_t model, Vector3& position) {
            return ReadDescriptionPosition(model, rr::SafeDoorModel::Description,
                rr::SafeDoorModel::DescriptionPosition, position);
        }, out, out.containers);

    AddDictionary(world, rr::WorldEntitiesModel::StorageBoxes, Kind::StorageBox,
        [](std::uintptr_t model, Vector3& position) {
            return ReadDescriptionPosition(model, rr::StorageBoxModel::DescriptionModel,
                rr::StorageBoxModel::DescriptionPosition, position);
        }, out, out.containers);

    AddDictionary(world, rr::WorldEntitiesModel::Shelvings, Kind::Shelving,
        [](std::uintptr_t model, Vector3& position) {
            return ReadDescriptionPosition(model, rr::ShelvingModel::DescriptionModel,
                rr::ShelvingModel::DescriptionPosition, position);
        }, out, out.containers);

    // CollectionTables — inline loop to read cash reward
    {
        std::vector<RobRunMemory::DictEntry> entries;
        if (ReadCollection(world, rr::WorldEntitiesModel::CollectionTables, entries)) {
            for (const auto& entry : entries) {
                const std::uintptr_t model = entry.value;
                if (model <= 0x10000) continue;

                const std::uintptr_t desc = RobRunMemory::ReadPtr(
                    model + rr::CollectionTableModel::DescriptionModel);
                if (desc <= 0x10000) continue;

                Vector3 position{};
                if (!RobRunMemory::ReadVector3(desc + rr::CollectionTableModel::Position, position) ||
                    !ValidPosition(position))
                    continue;

                int cashAmount = 0;
                bool hasCash = false;
                const std::uintptr_t rewardModel = RobRunMemory::ReadPtr(
                    model + rr::CollectionTableModel::RewardModel);
                if (rewardModel > 0x10000) {
                    const std::uintptr_t data = RobRunMemory::ReadPtr(rewardModel + 0x10);
                    if (data > 0x10000) {
                        for (std::uintptr_t off : { (std::uintptr_t)0x18, (std::uintptr_t)0x20,
                                                     (std::uintptr_t)0x28, (std::uintptr_t)0x10 }) {
                            const std::uintptr_t child = RobRunMemory::ReadPtr(data + off);
                            if (child > 0x10000) {
                                int val = RobRunMemory::ReadInt32(child + rr::CashAmountData::Value);
                                if (val > 0 && val < 100000000) {
                                    cashAmount = val; hasCash = true; break;
                                }
                            }
                            int directVal = RobRunMemory::ReadInt32(data + off);
                            if (directVal > 0 && directVal < 100000000) {
                                cashAmount = directVal; hasCash = true; break;
                            }
                        }
                    }
                }

                Entity entity{};
                entity.id = entry.key;
                entity.kind = Kind::CollectionTable;
                entity.position = position;
                entity.cashAmount = cashAmount;
                entity.hasCashAmount = hasCash;
                entity.label = "Collection Table";
                out.entities.push_back(std::move(entity));
                ++out.containers;
                if (out.entities.size() >= 4096) break;
            }
        }
    }

    AddDictionary(world, rr::WorldEntitiesModel::PlotContainers, Kind::PlotContainer,
        [](std::uintptr_t model, Vector3& position) {
            return ReadDescriptionPosition(model, rr::PlotContainerModel::Description,
                rr::PlotContainerModel::DescriptionPosition, position);
        }, out, out.containers);

    // --- New dump: dropped bags, vehicle loot, cargo and loose loot ----
    AddDictionary(world, rr::WorldEntitiesModel::DroppedLootBags, Kind::DroppedBag,
        [](std::uintptr_t model, Vector3& position) {
            const std::uintptr_t transform = RobRunMemory::ReadPtr(
                model + rr::DroppedBagModel::Transform);
            if (ReadScalarPosition(transform, position)) return true;
            return ReadInlineField(model, rr::DroppedBagModel::InteractPosition, position);
        }, out, out.loots);

    AddDictionary(world, rr::WorldEntitiesModel::VehicleLoots, Kind::VehicleLoot,
        [](std::uintptr_t model, Vector3& position) {
            return ReadInlineField(model, rr::VehicleLootModel::InteractPosition, position);
        }, out, out.loots);

    AddDictionary(world, rr::WorldEntitiesModel::Cargos, Kind::Cargo,
        [](std::uintptr_t model, Vector3& position) {
            const std::uintptr_t transform = RobRunMemory::ReadPtr(
                model + rr::CargoModel::Transform);
            if (ReadVehiclePosition(transform, position)) return true;
            return ReadInlineField(model, rr::CargoModel::InteractPosition, position);
        }, out, out.loots);

    AddDictionary(world, rr::WorldEntitiesModel::PlotLoots, Kind::PlotLoot,
        [](std::uintptr_t model, Vector3& position) {
            return ReadInlineField(model, rr::PlotLootModel::Position, position);
        }, out, out.loots);

    AddDictionary(world, rr::WorldEntitiesModel::FurnitureLoots, Kind::FurnitureLoot,
        [](std::uintptr_t model, Vector3& position) {
            return ReadInlineField(model, rr::FurnitureLootModel::Position, position);
        }, out, out.loots);

    AddDictionary(world, rr::WorldEntitiesModel::BuildingLoots, Kind::BuildingLoot,
        [](std::uintptr_t model, Vector3& position) {
            return ReadInlineField(model, rr::BuildingLootModel::Position, position);
        }, out, out.loots);

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

                Entity entity{};
                entity.id = entry.key;
                entity.kind = Kind::CashLootNode;
                entity.position = position;
                entity.cashAmount = cashAmount;
                entity.hasCashAmount = hasCash;
                entity.label = "Cash Node";
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

    // MoneyWorkbenches — clean + dirty amounts
    {
        std::vector<RobRunMemory::DictEntry> entries;
        if (ReadCollection(world, rr::WorldEntitiesModel::MoneyWorkbenches, entries)) {
            for (const auto& entry : entries) {
                const std::uintptr_t model = entry.value;
                if (model <= 0x10000) continue;

                Vector3 position{};
                if (!ReadDescriptionPosition(model, rr::MoneyWorkbenchModel::DescriptionModel,
                    rr::MoneyWorkbenchModel::DescriptionPosition, position))
                    continue;

                int cashAmount = 0;
                bool hasCash = false;
                const std::uintptr_t data = RobRunMemory::ReadPtr(
                    model + rr::MoneyWorkbenchModel::Data);
                if (data > 0x10000) {
                    const std::uintptr_t cleanObj = RobRunMemory::ReadPtr(
                        data + rr::MoneyWorkbenchData::CleanAmount);
                    const std::uintptr_t dirtyObj = RobRunMemory::ReadPtr(
                        data + rr::MoneyWorkbenchData::DirtyAmount);
                    int cleanVal = (cleanObj > 0x10000)
                        ? RobRunMemory::ReadInt32(cleanObj + rr::CashAmountData::Value) : 0;
                    int dirtyVal = (dirtyObj > 0x10000)
                        ? RobRunMemory::ReadInt32(dirtyObj + rr::CashAmountData::Value) : 0;
                    cashAmount = cleanVal + dirtyVal;
                    hasCash = true;
                }

                Entity entity{};
                entity.id = entry.key;
                entity.kind = Kind::MoneyWorkbench;
                entity.position = position;
                entity.cashAmount = cashAmount;
                entity.hasCashAmount = hasCash;
                entity.label = "Money Table";
                out.entities.push_back(std::move(entity));
                ++out.valuables;
                if (out.entities.size() >= 4096) break;
            }
        }
    }

    // --- New dump: points of interest ----------------------------------
    AddDictionary(world, rr::WorldEntitiesModel::PointOfInterest, Kind::PointOfInterest,
        [](std::uintptr_t model, Vector3& position) {
            return ReadDescriptionPosition(model, rr::PointOfInterestModel::Description,
                rr::PointOfInterestModel::DescriptionPosition, position);
        }, out, out.pointsOfInterest);

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
