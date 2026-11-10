#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace RobRunInventory {

struct DecodedSlot {
    std::uint32_t raw = 0;
    int durability = 0;
    int payload = 0;
    int encodedItem = 0;
    int itemIndex = -1;
    bool empty = true;
};

struct Slot {
    std::wstring id;
    int index = -1;
    int quantity = 0;
    int durability = 0;
    int itemIndex = -1;
    bool empty = true;
};

struct ItemDefinition {
    std::wstring id;
    int index = -1;
    int stackSize = 0;
    int rarity = 0;
    bool stackable = false;
    int inventoryLines = 0;
};

struct State {
    bool valid = false;
    int columns = 0;
    int maxLines = 0;
    int baseLines = 0;
    int backpackLines = 0;
    int activeLines = 0;
    std::vector<Slot> slots;
    std::vector<ItemDefinition> catalog;
};

void Init(std::uintptr_t gameAssemblyBase);

// Decode the packed InventorySlot:
//   durability bits 0..3, payload bits 4..18, item bits 19..31.
DecodedSlot DecodeSlot(std::uint32_t raw);

// Reads the local character's CharacterInventoryModel. `readCatalog` performs
// a one-off, bounded class-name scan; leave it off for per-frame calls.
bool Read(std::uintptr_t localCharacterModel, State& out, bool readCatalog = false);

// Resolves Models.ReferencesProject.Descriptions.Inventory.
// InventoryItemDescriptions and enumerates its item list.
bool ReadCatalog(std::vector<ItemDefinition>& out);

} // namespace RobRunInventory
