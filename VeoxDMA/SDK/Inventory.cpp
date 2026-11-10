#include "pch.h"
#include "Inventory.h"
#include "RobRunSDK.h"
#include "RobRunMemory.h"
#include "Il2Cpp.h"
#include "Memory.h"
#include <algorithm>

namespace RobRunInventory {

void Init(std::uintptr_t gameAssemblyBase)
{
    Il2Cpp::Init(gameAssemblyBase);
}

DecodedSlot DecodeSlot(std::uint32_t raw)
{
    DecodedSlot decoded{};
    decoded.raw = raw;
    decoded.durability = static_cast<int>(raw & rr::PackedSlot::DurabilityMask);
    decoded.payload = static_cast<int>((raw >> rr::PackedSlot::PayloadShift) & rr::PackedSlot::PayloadMask);
    decoded.encodedItem = static_cast<int>((raw >> rr::PackedSlot::ItemShift) & rr::PackedSlot::ItemMask);
    decoded.empty = decoded.encodedItem == 0;
    decoded.itemIndex = decoded.empty ? -1 : decoded.encodedItem - 1;
    return decoded;
}

namespace {

std::uintptr_t ReadFieldPtr(std::uintptr_t object, const char* fieldName,
    std::uintptr_t fallbackOffset)
{
    if (!object) return 0;
    const std::uintptr_t klass = Il2Cpp::ClassOf(object);
    Il2Cpp::Field field{};
    if (klass && Il2Cpp::FindField(klass, fieldName, field, true) &&
        field.offset > 0 && field.offset < 0x1000) {
        return RobRunMemory::ReadPtr(object + static_cast<std::uintptr_t>(field.offset));
    }
    return fallbackOffset ? RobRunMemory::ReadPtr(object + fallbackOffset) : 0;
}

std::uint32_t ReadPackedValue(std::uintptr_t dataObject)
{
    if (!dataObject) return 0;
    const std::uintptr_t klass = Il2Cpp::ClassOf(dataObject);
    Il2Cpp::Field field{};
    if (klass && Il2Cpp::FindField(klass, "_value", field, true) &&
        field.offset > 0 && field.offset < 0x1000) {
        return RobRunMemory::ReadUInt32(dataObject + static_cast<std::uintptr_t>(field.offset));
    }
    return RobRunMemory::ReadUInt32(dataObject + rr::InventorySlotData::Value);
}

bool ReadDefinition(std::uintptr_t definition, ItemDefinition& out)
{
    out = ItemDefinition{};
    if (definition <= 0x10000) return false;

    const std::uintptr_t idString = RobRunMemory::ReadPtr(definition + rr::InventoryItemDescription::Id);
    RobRunMemory::ReadString(idString, out.id);

    out.index = RobRunMemory::ReadInt32(definition + rr::InventoryItemDescription::Index);
    out.stackSize = RobRunMemory::ReadInt32(definition + rr::InventoryItemDescription::StackSize);
    out.rarity = RobRunMemory::ReadInt32(definition + rr::InventoryItemDescription::Rarity);
    out.inventoryLines = RobRunMemory::ReadInt32(definition + rr::InventoryItemDescription::InventoryLines);
    out.stackable = RobRunMemory::ReadUInt32(definition + rr::InventoryItemDescription::IsStackable) != 0;

    return out.index >= 0 || !out.id.empty();
}

std::uintptr_t ReadBackpackDefinition(std::uintptr_t inventory)
{
    const std::uintptr_t equip = RobRunMemory::ReadPtr(inventory + rr::CharacterInventoryModel::Backpack);
    if (equip <= 0x10000) return 0;

    // EquipSlotModel -> ItemModel -> _data -> definition pointer. The exact
    // field names differ between inflated generic wrappers, so try the common
    // paths and validate the result as an InventoryItemDescription.
    const std::uintptr_t candidates[] = {
        RobRunMemory::ReadPtr(equip + 0x10),
        equip,
        RobRunMemory::ReadPtr(equip + 0x18)
    };

    for (std::uintptr_t item : candidates) {
        if (item <= 0x10000) continue;
        const std::uintptr_t data = ReadFieldPtr(item, "_data", rr::InventoryItemModel::Data);
        if (data <= 0x10000) continue;
        const std::uintptr_t definition = ReadFieldPtr(data, "_value", 0);
        if (definition <= 0x10000) continue;
        if (Il2Cpp::IsClass(definition, "InventoryItemDescription"))
            return definition;
        // Some inflated wrappers store the definition inline at +0x18.
        const std::uintptr_t inlineDefinition = RobRunMemory::ReadPtr(data + rr::InventorySlotData::Value);
        if (inlineDefinition > 0x10000 &&
            Il2Cpp::IsClass(inlineDefinition, "InventoryItemDescription")) {
            return inlineDefinition;
        }
    }
    return 0;
}

} // namespace

bool Read(std::uintptr_t localCharacterModel, State& out, bool readCatalog)
{
    out = State{};
    if (localCharacterModel <= 0x10000) return false;

    const std::uintptr_t privateModel = RobRunMemory::ReadPtr(
        localCharacterModel + rr::BattleCharacterModel::PrivateModel);
    if (privateModel <= 0x10000) return false;

    const std::uintptr_t inventory = RobRunMemory::ReadPtr(
        privateModel + rr::BattleCharacterPrivateModel::Inventory);
    if (inventory <= 0x10000) return false;

    out.valid = true;

    const std::uintptr_t cells = RobRunMemory::ReadPtr(
        inventory + rr::CharacterInventoryModel::CellsSize);
    if (cells > 0x10000) {
        out.columns = RobRunMemory::ReadInt32(cells + rr::InventoryCellsSizeDescription::Columns);
        out.maxLines = RobRunMemory::ReadInt32(cells + rr::InventoryCellsSizeDescription::MaximumLines);
    }
    out.baseLines = RobRunMemory::ReadInt32(inventory + rr::CharacterInventoryModel::BaseLines);

    const std::uintptr_t backpack = ReadBackpackDefinition(inventory);
    if (backpack > 0x10000)
        out.backpackLines = RobRunMemory::ReadInt32(backpack + rr::InventoryItemDescription::InventoryLines);

    if (out.columns < 0 || out.columns > 64) out.columns = 0;
    if (out.maxLines < 0 || out.maxLines > 256) out.maxLines = 0;
    if (out.baseLines < 0 || out.baseLines > 256) out.baseLines = 0;
    if (out.backpackLines < 0 || out.backpackLines > 256) out.backpackLines = 0;

    out.activeLines = out.baseLines + out.backpackLines;
    if (out.maxLines > 0) out.activeLines = std::min(out.activeLines, out.maxLines);

    const std::uintptr_t slotsModel = RobRunMemory::ReadPtr(
        inventory + rr::InventorySlotsModel::Models);
    if (slotsModel > 0x10000) {
        const std::uintptr_t models = ReadFieldPtr(slotsModel, "_models", rr::InventorySlotsModel::Models);
        std::vector<RobRunMemory::RefDictEntry> entries;
        if (RobRunMemory::ReadDictRef(models, entries)) {
            out.slots.reserve(entries.size());
            for (const auto& entry : entries) {
                const std::uintptr_t description = entry.key;
                const std::uintptr_t slotModel = entry.value;
                if (description <= 0x10000 || slotModel <= 0x10000) continue;

                Slot slot{};
                RobRunMemory::ReadString(
                    RobRunMemory::ReadPtr(description + rr::InventorySlotDescription::Id), slot.id);
                slot.index = RobRunMemory::ReadInt32(description + rr::InventorySlotDescription::Index);

                const std::uintptr_t item = RobRunMemory::ReadPtr(
                    slotModel + rr::InventorySlotModel::Item);
                const std::uintptr_t stack = RobRunMemory::ReadPtr(
                    slotModel + rr::InventorySlotModel::Stack);

                std::uintptr_t data = ReadFieldPtr(item, "_data", rr::InventoryItemModel::Data);
                if (data <= 0x10000)
                    data = ReadFieldPtr(stack, "_data", rr::InventoryStackModel::Data);

                const DecodedSlot decoded = DecodeSlot(ReadPackedValue(data));
                slot.empty = decoded.empty;
                slot.durability = decoded.durability;
                slot.itemIndex = decoded.itemIndex;
                slot.quantity = decoded.empty ? 0 : (decoded.payload > 0 ? decoded.payload : 1);

                out.slots.push_back(std::move(slot));
            }
        }
    }

    if (readCatalog)
        ReadCatalog(out.catalog);

    return true;
}

bool ReadCatalog(std::vector<ItemDefinition>& out)
{
    out.clear();

    const std::uintptr_t klass = Il2Cpp::FindClassByName("InventoryItemDescriptions");
    if (!klass) return false;

    const std::uintptr_t statics = Il2Cpp::StaticFields(klass);
    if (!statics) return false;

    const std::uintptr_t cash = RobRunMemory::ReadPtr(
        statics + rr::InventoryItemDescriptions::StaticCash1);
    if (cash > 0x10000) {
        ItemDefinition definition{};
        if (ReadDefinition(cash, definition))
            out.push_back(std::move(definition));
    }

    const std::uintptr_t itemEnum = RobRunMemory::ReadPtr(
        statics + rr::InventoryItemDescriptions::StaticItemEnum);
    if (itemEnum <= 0x10000) return !out.empty();

    Il2Cpp::Field itemsField{};
    const std::uintptr_t enumClass = Il2Cpp::ClassOf(itemEnum);
    if (!enumClass || !Il2Cpp::FindField(enumClass, "_items", itemsField, true) ||
        itemsField.offset <= 0 || itemsField.offset >= 0x1000) {
        return !out.empty();
    }

    const std::uintptr_t list = RobRunMemory::ReadPtr(
        itemEnum + static_cast<std::uintptr_t>(itemsField.offset));
    std::vector<std::uintptr_t> definitions;
    if (!RobRunMemory::ReadList(list, definitions)) return !out.empty();

    out.reserve(definitions.size());
    for (std::uintptr_t definition : definitions) {
        ItemDefinition item{};
        if (ReadDefinition(definition, item))
            out.push_back(std::move(item));
    }
    return !out.empty();
}

} // namespace RobRunInventory
