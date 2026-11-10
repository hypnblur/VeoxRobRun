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
    case Kind::Container:  return "Container";
    case Kind::Loot:       return "Loot";
    case Kind::StaticLoot: return "Static Loot";
    case Kind::RandomLoot: return "Loot";
    case Kind::Vehicle:    return "Vehicle";
    case Kind::Car:        return "Car";
    case Kind::Helicopter: return "Helicopter";
    case Kind::Grenade:    return "Grenade";
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

bool WorldLooksValid(std::uintptr_t world)
{
    if (world <= 0x10000) return false;

    // Probe a few collections that exist in every loaded scene. A wrong base
    // pointer normally fails the dictionary structure checks.
    std::vector<RobRunMemory::DictEntry> entries;
    return RobRunMemory::ReadDictInt(world + rr::WorldEntitiesModel::Containers, entries) ||
        RobRunMemory::ReadDictInt(world + rr::WorldEntitiesModel::Loots, entries) ||
        RobRunMemory::ReadDictInt(world + rr::WorldEntitiesModel::Vehicles, entries);
}

void AddDictionary(std::uintptr_t world, std::uintptr_t dictionaryOffset, Kind kind,
    const PositionReader& readPosition, State& out, int& kindCounter)
{
    std::vector<RobRunMemory::DictEntry> entries;
    if (!RobRunMemory::ReadDictInt(world + dictionaryOffset, entries)) return;

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

bool Read(std::uintptr_t battleModel, State& out)
{
    out = State{};
    if (battleModel <= 0x10000) return false;

    std::uintptr_t world = RobRunMemory::ReadPtr(
        battleModel + rr::BattleModel::WorldEntitiesModel);
    if (!WorldLooksValid(world)) {
        const std::uintptr_t legacy = RobRunMemory::ReadPtr(
            battleModel + rr::BattleModel::WorldEntitiesModelLegacy);
        if (WorldLooksValid(legacy))
            world = legacy;
    }
    if (world <= 0x10000) return false;

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
    if (RobRunMemory::ReadDictInt(world + rr::WorldEntitiesModel::Containers, containerEntries)) {
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

    AddDictionary(world, rr::WorldEntitiesModel::Loots, Kind::Loot,
        [](std::uintptr_t model, Vector3& position) {
            const std::uintptr_t transform = RobRunMemory::ReadPtr(
                model + rr::LootModel::Transform);
            return ReadScalarPosition(transform, position);
        }, out, out.loots);

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

    AddDictionary(world, rr::WorldEntitiesModel::Vehicles, Kind::Vehicle,
        [](std::uintptr_t model, Vector3& position) {
            const std::uintptr_t transform = RobRunMemory::ReadPtr(
                model + rr::VehicleModel::Transform);
            return ReadVehiclePosition(transform, position);
        }, out, out.vehicles);

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

    return out.valid;
}

} // namespace RobRunWorld
