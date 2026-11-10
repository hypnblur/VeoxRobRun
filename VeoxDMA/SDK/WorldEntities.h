#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../Misc/Vector.h"

namespace RobRunWorld {

enum class Kind {
    Container,
    Loot,
    StaticLoot,
    RandomLoot,
    Vehicle,
    Car,
    Helicopter,
    Grenade
};

struct Entity {
    int id = 0;
    Kind kind = Kind::Container;
    Vector3 position{};
    bool hasEmptyFlag = false;
    bool empty = false;
    std::string label;
};

struct State {
    bool valid = false;
    std::vector<Entity> entities;
    int containers = 0;
    int loots = 0;
    int vehicles = 0;
    int grenades = 0;
};

void Init(std::uintptr_t gameAssemblyBase);

// Reads WorldEntitiesModel. The dump places it at BattleEntitiesModel+0x60;
// BattleModel+0x60 and the legacy BattleModel+0x510 are validated fallbacks.
bool Read(std::uintptr_t battleModel, std::uintptr_t battleEntities, State& out);

const char* KindName(Kind kind);

} // namespace RobRunWorld
