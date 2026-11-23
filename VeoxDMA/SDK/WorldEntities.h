#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_set>
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
    Grenade,
    LootNode,
    SafeDoor,
    StorageBox,
    Shelving,
    CollectionTable,
    PlotContainer,
    DroppedBag,
    VehicleLoot,
    Cargo,
    CargoBush,
    CargoCrystal,
    CargoDrug,
    CargoCash,
    CargoExchange,
    CargoBox,
    CargoGoods,
    PlotLoot,
    FurnitureLoot,
    BuildingLoot,
    CashLootNode,
    SettlementCash,
    MoneyWorkbench,
    SecurityAlarm,
    LocationSecurityAlarm,
    SecurityPanel,
    MetalDetector,
    FirstAidCabinet,
    CryptoWorkbench,
    VehicleTrunk,
    DrugWorkbench,
    DefenseBlock
};

struct Entity {
    int id = 0;
    Kind kind = Kind::Container;
    Vector3 position{};
    bool hasEmptyFlag = false;
    bool empty = false;
    std::string label;
    int cashAmount = 0;
    bool hasCashAmount = false;
        bool inVehicle = false;     // contents of a vehicle trunk
        bool inTable = false;       // contents of a container or grow table
    int dirtyCash = 0;
    int cleanCash = 0;
    int value = 0;              // game-native value (cargo coin reward), 0 if none
    bool hasValue = false;
    int rarity = -1;            // RarityItem enum from the description: 0 none, 1..6 tiers
    float yaw = 0.f;
    bool hasYaw = false;
    // Resolved once per world-worker pass into LootFilter::kItemCatalog
    // (-2 = unresolved, -1 = resolved with no match, >= 0 = catalog index).
    std::int16_t catalogIndex = -2;
};

struct State {
    bool valid = false;
    std::vector<Entity> entities;
    int containers = 0;
    int loots = 0;
    int vehicles = 0;
    int grenades = 0;
    int valuables = 0;
    int alarms = 0;
    int workbenches = 0;
    int firstAid = 0;
    int drugWorkbenches = 0;
    int defenseBlocks = 0;
    std::unordered_set<int> hiddenEntityIds;
    std::uintptr_t worldPtr = 0;
};

// House / plot info for the ESP info panel
struct BuildingInfo {
    std::string name;
    bool hasDefense = false;
    bool isRaided = false;
    bool isFusesEmpty = false;
    std::int64_t raidLockTs = 0;
    int doorCount = 0;
    int doorsOpen = 0;
};

struct PlotInfo {
    int plotId = 0;
    Vector3 position{};
    std::string ownerName;
    std::string ownerClan;
    std::string ownerClanTag;
    int slotIndex = -1;
    BuildingInfo mainBuilding;
    BuildingInfo compoundWall;
    std::vector<BuildingInfo> outbuildings;
};

struct PlotState {
    bool valid = false;
    std::vector<PlotInfo> plots;
};

struct ProbeField {
    std::string name;
    std::int32_t offset = 0;
    std::uint8_t typeCode = 0;
    std::int64_t rawValue = 0;
};

struct ProbeResult {
    std::string dictName;
    std::string className;
    std::uintptr_t ptr = 0;
    std::vector<ProbeField> fields;
};

std::vector<ProbeResult> ProbeMoneyModels(std::uintptr_t worldPtr);

void Init(std::uintptr_t gameAssemblyBase);

// Reads WorldEntitiesModel. The dump places it at BattleModel+0x510;
// BattleEntitiesModel+0x60 and BattleModel+0x60 are validated fallbacks.
bool Read(std::uintptr_t battleModel, std::uintptr_t battleEntities, State& out);

const char* KindName(Kind kind);

bool ReadPlots(std::uintptr_t battleEntities, PlotState& out);

} // namespace RobRunWorld
