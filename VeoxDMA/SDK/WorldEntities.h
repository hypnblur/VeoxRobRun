#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <unordered_set>
#include "../Misc/Vector.h"

namespace RobRunMemory { struct DictEntry; }

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

struct GrenadeSample {
    int id = 0;
    std::uintptr_t model = 0;
    std::uintptr_t transform = 0;
    std::uintptr_t description = 0;
    Vector3 position{};
    float effectRadius = 0.f;
    std::optional<Vector3> contact;
    bool settled = false;
};

struct GrenadeContactLayout {
    std::uintptr_t flyPhysics = 0;
    std::uintptr_t isSleeping = 0;
    std::uintptr_t clientCollision = 0;
    std::uintptr_t hasCollision = 0;
    std::uintptr_t collisionPoint = 0;
};

std::uintptr_t FindWorldEntities(std::uintptr_t battleModel, std::uintptr_t battleEntities);
bool ReadGrenadeSamples(std::uintptr_t world, const std::vector<RobRunMemory::DictEntry>& entities,
    GrenadeContactLayout& layout, std::vector<GrenadeSample>& out, std::size_t& discovered);
float ReadGrenadeEffectRadius(std::uintptr_t description);

enum class WorkbenchKind { GrowthTable, LabFreezer, ChemicalReactor };

struct WorkbenchCargo {
    std::string id;
    int rarity = 0;
    int count = 0;
};

struct WorkbenchInfo {
    WorkbenchKind kind = WorkbenchKind::GrowthTable;
    int entityId = -1;
    Vector3 position{};
    bool hasPosition = false;
    bool contentsAvailable = false;
    std::vector<WorkbenchCargo> cargo;
};

// House / plot info shared by the popup and the 3D overlay.
struct BuildingInfo {
    std::string name;
    bool hasDefense = false;
    bool isRaided = false;
    bool isFusesEmpty = false;
    std::int64_t raidLockTs = 0;
    int doorCount = 0;
    int doorsOpen = 0;
    bool isWorkbenchRoom = false;
    bool workbenchesUnavailable = false;
    std::vector<WorkbenchInfo> workbenches;
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

bool ReadPlots(std::uintptr_t battleEntities, const Vector3& localPosition, PlotState& out);

} // namespace RobRunWorld
