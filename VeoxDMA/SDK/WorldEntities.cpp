#include "pch.h"
#include "WorldEntities.h"
#include "../Config/LootFilterData.h"
#include "../Config/ConfigInstance.h"
#include "Diag.h"
#include "RobRunSDK.h"
#include "RobRunMemory.h"
#include "Il2Cpp.h"
#include "Memory.h"
#include "DebugLog.h"
#include <cmath>
#include <functional>
#include <unordered_map>
#include <unordered_set>


// Trunk/table inventory walks stay off the hot path: the slot layout is still
// unpinned and each slot costs a DMA read on every world refresh.
inline constexpr bool kReadInventories = false;

namespace RobRunWorld {

namespace { void ResetLiveItemCache(); }

void Init(std::uintptr_t gameAssemblyBase)
{
    Il2Cpp::Init(gameAssemblyBase);
    ResetLiveItemCache();
}

static bool IsPaintingId(std::string_view id)
{
    return id.starts_with("original_picture_") || id.starts_with("trash_picture_") ||
        id.starts_with("picture_tube_") || id.starts_with("picture_fake_");
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
    case Kind::CargoBush:       return "Bush";
    case Kind::CargoCrystal:    return "Crystal";
    case Kind::CargoDrug:       return "Drug";
    case Kind::CargoCash:       return "Cash Cargo";
    case Kind::CargoExchange:   return "Exchange";
    case Kind::CargoBox:        return "Cargo Box";
    case Kind::CargoGoods:      return "Goods";
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

Kind ClassifyCargo(const std::string& name)
{
    if (name.find("Bush") != std::string::npos || name.find("Datura") != std::string::npos)
        return Kind::CargoBush;
    if (name.find("Crystal") != std::string::npos)
        return Kind::CargoCrystal;
    if (name.find("Drug") != std::string::npos || name.find("Reagent") != std::string::npos)
        return Kind::CargoDrug;
    if (name.find("Cash") != std::string::npos || name.find("Gold") != std::string::npos)
        return Kind::CargoCash;
    if (name.find("Exchange") != std::string::npos)
        return Kind::CargoExchange;
    if (name.find("Box") != std::string::npos || name.find("Case") != std::string::npos ||
        name.find("Military") != std::string::npos)
        return Kind::CargoBox;
    if (name.find("Electronics") != std::string::npos || name.find("Food") != std::string::npos ||
        name.find("Material") != std::string::npos || name.find("Medical") != std::string::npos)
        return Kind::CargoGoods;
    return Kind::Cargo;
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

// A plain identifier: printable, not a bare number. Numeric "labels" mean the
// wrong field was read, so they are rejected rather than shown on screen.
bool LooksLikeId(const std::string& s)
{
    if (s.empty() || s.size() > 64) return false;
    bool hasAlpha = false;
    for (unsigned char c : s) {
        if (c < 0x20 || c > 0x7E) return false;
        if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') hasAlpha = true;
    }
    return hasAlpha;
}

// Func<T> held at obj+offset. Il2CppDelegate layout: +0x10 method_ptr (code),
// +0x18 invoke_impl, +0x20 target (the closure that holds the captured value).
// Reading +0x10 yields code bytes, which is why the old version returned the
// same number for every item.
std::uintptr_t ReadFuncTarget(std::uintptr_t obj, std::uintptr_t funcOffset)
{
    if (obj <= 0x10000) return 0;
    const std::uintptr_t fn = RobRunMemory::ReadPtr(obj + funcOffset);
    if (fn <= 0x10000) return 0;
    const std::uintptr_t target = RobRunMemory::ReadPtr(fn + 0x20);
    if (target <= 0x10000) return 0;
    return RobRunMemory::ReadPtr(target + 0x10);
}

// LootObjectItemDescription -> InventoryItemDescription. The tiered item id
// ("starh_m2") and the rarity live there; the loot-object id ("star_h_m_2")
// only names the generic pickup. The closure keeps its captured value at 0x10
// on most delegates and 0x18 on others, so both are tried and the first that
// reads back a plausible identifier wins.
std::uintptr_t ReadFuncIdentifiedTarget(std::uintptr_t obj, std::uintptr_t funcOffset)
{
    if (obj <= 0x10000) return 0;
    const std::uintptr_t fn = RobRunMemory::ReadPtr(obj + funcOffset);
    if (fn <= 0x10000) return 0;
    const std::uintptr_t target = RobRunMemory::ReadPtr(fn + 0x20);
    if (target <= 0x10000) return 0;

    std::uintptr_t first = 0;
    for (const std::uintptr_t valueOffset : { (std::uintptr_t)0x10, (std::uintptr_t)0x18 }) {
        const std::uintptr_t value = RobRunMemory::ReadPtr(target + valueOffset);
        if (value <= 0x10000) continue;
        if (!first) first = value;
        if (LooksLikeId(ReadIdentifiedId(value))) return value;
    }
    return first;
}


// Is this id one the loot catalog knows as an item? Loot-object ids and item
// ids both look like identifiers, so the catalog is what tells them apart.
bool IsKnownItemId(const std::string& id)
{
    static std::unordered_set<std::string> s_ids;
    if (s_ids.empty()) {
        for (const auto& def : LootFilter::kItemCatalog) {
            if (!def.labelMatch[0]) continue;
            std::string key(def.labelMatch);
            std::transform(key.begin(), key.end(), key.begin(),
                [](unsigned char c) { return (char)std::tolower(c); });
            s_ids.insert(std::move(key));
        }
    }
    if (id.empty()) return false;
    std::string key(id);
    std::transform(key.begin(), key.end(), key.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return s_ids.count(key) > 0;
}

// Func<int>: InventoryItemDescription.<>c__DisplayClass327_0 holds <>4__this at
// +0x10 and `int basePrice` at +0x18. Check +0x18 first so we never mistake the
// low 32 bits of the <>4__this pointer for a price.
bool ReadFuncInt(std::uintptr_t obj, std::uintptr_t funcOffset, int& out)
{
    if (obj <= 0x10000) return false;
    const std::uintptr_t fn = RobRunMemory::ReadPtr(obj + funcOffset);
    if (fn <= 0x10000) return false;
    const std::uintptr_t target = RobRunMemory::ReadPtr(fn + 0x20);   // delegate target, not method_ptr
    if (target <= 0x10000) return false;
    for (const std::uintptr_t off : { (std::uintptr_t)0x18, (std::uintptr_t)0x10 }) {
        const int value = RobRunMemory::ReadInt32(target + off);
        if (value > 0 && value < 100000000) { out = value; return true; }
    }
    return false;
}

// InventoryItemDescription carries BasePriceFunc (0x78) and
// SellPriceModifierFunc (0x80 -> InventoryItemSellPriceModifier -> float
// Modifier at +0x20). Note that SellPriceFunc (0x88) shares the same closure
// as BasePriceFunc (holding only basePrice, not the multiplied sell price),
// so the real sell price is round(basePrice * modifier).
bool ReadItemPrices(std::uintptr_t itemDesc, int& basePrice, int& sellPrice)
{
    basePrice = 0;
    sellPrice = 0;
    if (itemDesc <= 0x10000) return false;
    const bool haveBase = ReadFuncInt(itemDesc, rr::InventoryItemDescriptionModel::BasePriceFunc, basePrice);
    if (!haveBase || basePrice <= 0) return false;

    const std::uintptr_t modObj = ReadFuncTarget(
        itemDesc, rr::InventoryItemDescriptionModel::SellPriceModifierFunc);
    if (modObj > 0x10000) {
        const float mod = RobRunMemory::ReadFloat(
            modObj + rr::InventoryItemSellPriceModifierModel::Modifier);
        if (std::isfinite(mod) && mod >= 0.f && mod <= 10.f) {
            sellPrice = static_cast<int>(std::lround(
                static_cast<double>(basePrice) * static_cast<double>(mod)));
            return true;
        }
    }
    sellPrice = basePrice;
    return true;
}

struct LiveItemInfo {
    int basePrice = 0;
    int sellPrice = 0;
    int rarity = 0;
};

static std::unordered_map<std::string, LiveItemInfo> g_liveItems;
static std::uint64_t g_livePricesLastTryMs = 0;

void ResetLiveItemCache()
{
    g_liveItems.clear();
    g_livePricesLastTryMs = 0;
}

void EnsureLiveItemPrices()
{
    if (!g_liveItems.empty()) return;
    const std::uint64_t now = GetTickCount64();
    if (g_livePricesLastTryMs != 0 && (now - g_livePricesLastTryMs) < 5000) return;
    g_livePricesLastTryMs = now;

    const std::uintptr_t klass = Il2Cpp::FindClassByName("InventoryItemDescriptions");
    if (klass <= 0x10000) return;
    const std::uintptr_t statics = Il2Cpp::StaticFields(klass);
    if (statics <= 0x10000) return;

    const std::uintptr_t enumObj = RobRunMemory::ReadPtr(statics + 0x0);
    if (enumObj <= 0x10000) return;
    const std::uintptr_t listObj = RobRunMemory::ReadPtr(enumObj + 0x10); // ReadOnlyEnum<T>._items
    if (listObj <= 0x10000) return;

    const std::uintptr_t arr = RobRunMemory::ReadPtr(listObj + rr::List::items);
    const std::int32_t count = RobRunMemory::ReadInt32(listObj + rr::List::count);
    if (arr <= 0x10000 || count <= 0 || count > 4096) return;

    std::unordered_map<std::string, LiveItemInfo> loaded;
    loaded.reserve(static_cast<std::size_t>(count));
    for (std::int32_t i = 0; i < count; ++i) {
        const std::uintptr_t itemDesc = RobRunMemory::ReadPtr(
            arr + rr::Array::vector + static_cast<std::uintptr_t>(i) * rr::Array::stride);
        if (itemDesc <= 0x10000) continue;
        std::string id = ReadIdentifiedId(itemDesc);
        if (id.empty()) continue;
        std::transform(id.begin(), id.end(), id.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        int basePrice = 0, sellPrice = 0;
        ReadItemPrices(itemDesc, basePrice, sellPrice);
        const int rarity = RobRunMemory::ReadInt32(
            itemDesc + rr::InventoryItemDescriptionModel::Rarity);
        if (basePrice > 0 || sellPrice > 0 || (rarity > 0 && rarity <= 6)) {
            loaded[id] = { basePrice, sellPrice, (rarity > 0 && rarity <= 6) ? rarity : 0 };
        }
    }
    if (!loaded.empty())
        g_liveItems = std::move(loaded);
}

bool LookupLiveItemInfo(const std::string& label, LiveItemInfo& outInfo)
{
    if (label.empty() || g_liveItems.empty()) return false;
    std::string key = label;
    std::transform(key.begin(), key.end(), key.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto it = g_liveItems.find(key);
    if (it != g_liveItems.end()) {
        outInfo = it->second;
        return true;
    }
    return false;
}

// Resolve a typed description delegate without executing it in the game.
// Handles both direct closures (<>c__DisplayClass3_0) and indexed registry
// closures (<>c__DisplayClass5_0`1 capturing Enum<T> at +0x10 and index at +0x18).
std::uintptr_t ResolveDescriptionDelegate(std::uintptr_t func, const char* directClosure,
    const char* captureName)
{
    if (!Il2Cpp::IsClass(func, "Func`1")) return 0;

    constexpr std::uintptr_t delegateTarget = 0x20;
    const auto target = RobRunMemory::ReadPtr(func + delegateTarget);
    const std::string targetClass = Il2Cpp::ClassNameOf(target);

    if (targetClass == directClosure) {
        Il2Cpp::Field capture{};
        if (!Il2Cpp::FindField(Il2Cpp::ClassOf(target), captureName, capture) ||
            capture.offset != 0x10)
            return 0;
        return RobRunMemory::ReadPtr(target + capture.offset);
    }
    if (targetClass != "<>c__DisplayClass5_0`1") return 0;

    constexpr std::uintptr_t capturedEnum = 0x10;
    constexpr std::uintptr_t capturedIndex = 0x18;
    constexpr std::uintptr_t enumItems = 0x10;
    const auto registry = RobRunMemory::ReadPtr(target + capturedEnum);
    if (!Il2Cpp::IsClass(registry, "Enum`1")) return 0;
    const auto list = RobRunMemory::ReadPtr(registry + enumItems);
    if (!Il2Cpp::IsClass(list, "List`1")) return 0;

    std::int32_t index = -1;
    std::int32_t count = 0;
    if (!RobRunMemory::ReadBytes(target + capturedIndex, &index, sizeof(index)) ||
        !RobRunMemory::ReadBytes(list + rr::List::count, &count, sizeof(count)) ||
        index < 0 || index >= count)
        return 0;

    const auto array = RobRunMemory::ReadPtr(list + rr::List::items);
    std::uint64_t length = 0;
    if (array <= 0x10000 ||
        !RobRunMemory::ReadBytes(array + rr::Array::max_length, &length, sizeof(length)) ||
        static_cast<std::uint64_t>(count) > length)
        return 0;

    return RobRunMemory::ReadPtr(array + rr::Array::vector +
        static_cast<std::uintptr_t>(index) * rr::Array::stride);
}

std::uintptr_t ResolveInventoryItemDescription(std::uintptr_t lootObjectItem)
{
    if (lootObjectItem <= 0x10000) return 0;
    if (Il2Cpp::IsClass(lootObjectItem, "LootObjectItemDescription")) {
        const auto func = RobRunMemory::ReadPtr(
            lootObjectItem + rr::LootObjectItemDescriptionModel::ItemDescriptionFunc);
        const auto item = ResolveDescriptionDelegate(func, "<>c__DisplayClass3_0", "itemDescription");
        if (Il2Cpp::IsClassOrDerived(item, "InventoryItemDescription"))
            return item;
    }
    return ReadFuncTarget(lootObjectItem,
        rr::LootObjectItemDescriptionModel::ItemDescriptionFunc);
}

// Deeper variant: a LootObjectItemDescription reaches its InventoryItemDescription
// through ItemDescriptionFunc, but on some wrappers the description sits one
// level down instead. Try the func, then each slot, validating against a real
// identifier so a wrong hop can never be mistaken for a hit.
std::uintptr_t ResolveItemDescriptionDeep(std::uintptr_t lootObjectItem)
{
    if (lootObjectItem <= 0x10000) return 0;

    const std::uintptr_t direct = ReadFuncIdentifiedTarget(
        lootObjectItem, rr::LootObjectItemDescriptionModel::ItemDescriptionFunc);
    if (direct > 0x10000 && LooksLikeId(ReadIdentifiedId(direct)))
        return direct;

    for (const std::uintptr_t off : { (std::uintptr_t)0x10, (std::uintptr_t)0x18,
                                      (std::uintptr_t)0x20, (std::uintptr_t)0x28 }) {
        const std::uintptr_t value = RobRunMemory::ReadPtr(lootObjectItem + off);
        if (value <= 0x10000) continue;
        if (LooksLikeId(ReadIdentifiedId(value))) return value;

        const std::uintptr_t nested = ReadFuncIdentifiedTarget(value,
            rr::LootObjectItemDescriptionModel::ItemDescriptionFunc);
        if (nested > 0x10000 && LooksLikeId(ReadIdentifiedId(nested))) return nested;
    }
    return 0;
}

// One-shot pointer walk. Generic wrappers (IPrimitiveData<T>, Func<T>) report
// 0x0 field offsets in the dump, so the only honest way to learn a hop is to
// print the words and the strings they point at, once, from a live session.
std::string ShortString(std::uintptr_t obj)
{
    if (obj <= 0x10000) return {};
    std::wstring wide;
    if (!RobRunMemory::ReadString(obj, wide, 32)) return {};
    std::string s = RobRunMemory::WideToUtf8(wide);
    for (char& c : s)
        if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) c = '?';
    return s;
}

void ProbeWords(const char* tag, std::uintptr_t base, int count)
{
    if (!kProbes) return;   // instrumentation only; silent unless asked for
    if (base <= 0x10000) { VeoxDiag("%s: base=null", tag); return; }
    std::string line;
    for (int i = 0; i < count; ++i) {
        const std::uintptr_t v = RobRunMemory::ReadPtr(base + i * 8);
        const std::string s = ShortString(v);
        char buf[128];
        if (!s.empty())
            snprintf(buf, sizeof(buf), " +%02X=%llX('%s')", i * 8, (unsigned long long)v, s.c_str());
        else
            snprintf(buf, sizeof(buf), " +%02X=%llX", i * 8, (unsigned long long)v);
        line += buf;
    }
    VeoxDiag("%s:%s", tag, line.c_str());
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


// Loot objects stay in the world dictionary after being picked up; the model
// flips _isTaken (ILootObjectModel, IPrimitiveData<bool> at 0x10) instead of
// disappearing. Without this check the ESP keeps drawing loot that is gone.
// Two ids refer to the same thing once separators and case are folded
// ("alcohol_9" == "Alcohol_9" == "alcohol9").
bool LenientEquals(const std::string& a, const std::string& b)
{
    auto fold = [](const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (unsigned char c : s)
            if (std::isalnum(c)) out.push_back((char)std::tolower(c));
        return out;
    };
    return !a.empty() && !b.empty() && fold(a) == fold(b);
}

bool IsLootTaken(std::uintptr_t model)
{
    if (!ConfigInstance.LootFilter.FilterTakenLoot) return false;
    if (model <= 0x10000) return false;
    const std::uintptr_t wrapper = RobRunMemory::ReadPtr(model + 0x10);
    if (wrapper <= 0x10000) return false;
    // Guard the hop: a bool must come from a boolean primitive, otherwise a
    // misplaced pointer could report every object as already looted.
    const std::string className = Il2Cpp::ClassNameOfClass(Il2Cpp::ClassOf(wrapper));
    if (className.find("Bool") == std::string::npos &&
        className.find("bool") == std::string::npos)
        return false;
    bool taken = false;
    if (Il2Cpp::ReadPrimitiveBool(wrapper, taken)) return taken;
    return false;
}


// ---------------------------------------------------------------- inventories
// A trunk or a grow table stores its contents as a slot grid:
//   IInventoryModel (0x18) -> InventorySlotsModel (ReadOnlyDictionaryModel)
//     -> _models (0x10) : Dictionary<InventorySlotDescription, IInventorySlotModel>
// Every slot model holds a reference to an item description somewhere, but the
// concrete slot class differs per container, so instead of hardcoding one offset
// each slot's first pointers are checked and the first that yields a known item
// id wins. Validation is the guard: a wrong hop cannot pass it.
void EnumerateInventoryItems(std::uintptr_t inventory, std::size_t maxItems,
    const std::function<void(const std::string&, std::uintptr_t)>& emit)
{
    if (inventory <= 0x10000) return;
    const std::uintptr_t slots = RobRunMemory::ReadPtr(inventory + 0x18);
    if (slots <= 0x10000) return;
    const std::uintptr_t dictionary = RobRunMemory::ReadPtr(slots + 0x10);
    if (dictionary <= 0x10000) return;

    std::vector<RobRunMemory::RefDictEntry> entries;
    if (!RobRunMemory::ReadDictRef(dictionary, entries, false)) return;

    std::size_t emitted = 0;
    for (const auto& entry : entries) {
        if (entry.value <= 0x10000) continue;
        for (std::uintptr_t off = 0x10; off <= 0x60; off += 8) {
            const std::uintptr_t candidate = RobRunMemory::ReadPtr(entry.value + off);
            if (candidate <= 0x10000) continue;
            const std::string id = ReadIdentifiedId(candidate);
            if (!IsKnownItemId(id)) continue;
            emit(id, candidate);
            if (++emitted >= maxItems) return;
            break;
        }
    }
}

// Vehicle trunks: world + 0xC0 -> VehicleTrunkModel.Inventory (0x20).
void AddVehicleTrunkContents(std::uintptr_t world, std::vector<Entity>& items)
{
    std::vector<RobRunMemory::DictEntry> trunks;
    if (!ReadCollection(world, rr::WorldEntitiesModel::VehicleTrunks, trunks)) return;

    for (const auto& trunk : trunks) {
        if (trunk.value <= 0x10000) continue;
        Vector3 position{};
        const std::uintptr_t inventory = RobRunMemory::ReadPtr(trunk.value + 0x20);
        if (inventory <= 0x10000) continue;

        EnumerateInventoryItems(inventory, 24, [&](const std::string& id, std::uintptr_t) {
            Entity entity{};
            entity.id = trunk.key;
            entity.kind = Kind::Loot;
            entity.label = id;
            entity.inVehicle = true;
            items.push_back(std::move(entity));
        });
    }
}

// Plot containers / grow tables: world + 0x138 -> inventory model.
void AddPlotContainerContents(std::uintptr_t world, std::vector<Entity>& items)
{
    std::vector<RobRunMemory::DictEntry> containers;
    if (!ReadCollection(world, rr::WorldEntitiesModel::PlotContainers, containers)) return;

    for (const auto& entry : containers) {
        if (entry.value <= 0x10000) continue;
        std::uintptr_t inventory = RobRunMemory::ReadPtr(entry.value + 0x18);
        if (inventory <= 0x10000) inventory = entry.value;   // some models *are* the inventory

        EnumerateInventoryItems(inventory, 16, [&](const std::string& id, std::uintptr_t) {
            Entity entity{};
            entity.id = entry.key;
            entity.kind = Kind::Loot;
            entity.label = id;
            entity.inTable = true;
            items.push_back(std::move(entity));
        });
    }
}

bool IsLootKind(Kind kind)
{
    return kind == Kind::StaticLoot || kind == Kind::RandomLoot || kind == Kind::Loot ||
           kind == Kind::VehicleLoot || kind == Kind::DroppedBag;
}

void AddDictionary(std::uintptr_t world, std::uintptr_t dictionaryOffset, Kind kind,
    const PositionReader& readPosition, State& out, int& kindCounter)
{
    std::vector<RobRunMemory::DictEntry> entries;
    if (!ReadCollection(world, dictionaryOffset, entries)) return;

    for (const auto& entry : entries) {
        const std::uintptr_t model = entry.value;
        if (model <= 0x10000) continue;
        if (IsLootKind(kind) && IsLootTaken(model)) continue;

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
    const bool lootKindA = IsLootKind(kind);
    for (const auto& e : entries) {
        if (e.value <= 0x10000) continue;
        if (lootKindA && IsLootTaken(e.value)) continue;
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
            if (kind != Kind::RandomLoot) {
                std::string name = ReadIdentifiedId(item.desc);
                entity.label = LooksLikeId(name) ? std::move(name) : KindName(kind);
            }
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
    const bool lootKindB = IsLootKind(kind);
    for (const auto& e : entries) {
        if (e.value <= 0x10000) continue;
        if (lootKindB && IsLootTaken(e.value)) continue;
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

    // Vehicles describe themselves by model id ("fukurou_spark_9", ...) on the
    // description object. That id is what the loot filter matches an icon on,
    // so replace the generic "Car"/"Helicopter" label with it.
    {
        std::unordered_map<std::int32_t, std::size_t> idx;
        for (std::size_t i = 0; i < out.entities.size(); ++i) {
            const auto k = out.entities[i].kind;
            if (k == Kind::Car || k == Kind::Helicopter)
                idx[out.entities[i].id] = i;
        }
        auto labelVehicles = [&](std::uintptr_t dictOffset, std::uintptr_t descriptionOffset) {
            std::vector<RobRunMemory::DictEntry> entries;
            if (!ReadCollection(world, dictOffset, entries)) return;
            for (const auto& e : entries) {
                auto it = idx.find(e.key);
                if (it == idx.end() || e.value <= 0x10000) continue;
                const std::uintptr_t desc = RobRunMemory::ReadPtr(e.value + descriptionOffset);
                std::string id = ReadIdentifiedId(desc);
                if (!id.empty())
                    out.entities[it->second].label = std::move(id);
            }
        };
        labelVehicles(rr::WorldEntitiesModel::Cars, rr::CarModel::Description);
        labelVehicles(rr::WorldEntitiesModel::Helicopters, rr::HelicopterModel::Description);
    }

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

            // Container type lives behind LocationContainerDescription.ContainerDescriptionFunc;
            // the description object itself carries only an entity id.
            const std::uintptr_t desc = RobRunMemory::ReadPtr(
                entry.value + rr::ContainerModel::Description);
            const std::uintptr_t containerDesc = ReadFuncIdentifiedTarget(
                desc, rr::LocationContainerDescriptionModel::ContainerDescriptionFunc);
            const std::string id = ReadIdentifiedId(containerDesc);
            if (LooksLikeId(id))
                out.entities[it->second].label = id;
        }
    }

    // 0x38 is droppedLootBags in the current dump. It used to be read here as
    // the generic "loots" dictionary; reading it a second time under
    // DroppedBag below would duplicate every bag and double the DMA traffic,
    // so the DroppedBag pass is the only reader for this offset.

    EnsureLiveItemPrices();

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
                const std::uintptr_t selectedLoot = ResolveDescriptionDelegate(
                    func, "<>c__DisplayClass35_0", "lootObjectItemDescription");
                const std::string selectedId = Il2Cpp::IsClass(selectedLoot, "LootObjectItemDescription")
                    ? ReadIdentifiedId(selectedLoot) : std::string{};
                const std::uintptr_t target = RobRunMemory::ReadPtr(func + 0x20);
                const std::uintptr_t lootObjectItem = selectedId.empty() && target > 0x10000
                    ? RobRunMemory::ReadPtr(target + 0x10) : selectedLoot;
                if (lootObjectItem <= 0x10000) continue;

                const std::string lootId = ReadIdentifiedId(lootObjectItem);
                if (IsPaintingId(lootId)) {
                    auto& entity = out.entities[it->second];
                    entity.label = lootId;
                    const std::uintptr_t invDesc = ResolveInventoryItemDescription(lootObjectItem);
                    if (invDesc > 0x10000) {
                        const int r = RobRunMemory::ReadInt32(
                            invDesc + rr::InventoryItemDescriptionModel::Rarity);
                        if (r > 0 && r <= 6)
                            entity.rarity = r;
                        int basePrice = 0, sellPrice = 0;
                        if (ReadItemPrices(invDesc, basePrice, sellPrice) && sellPrice > 0) {
                            entity.value = sellPrice;
                            entity.hasValue = true;
                        }
                    }
                    continue;
                }

                // Second hop: the loot object names the pickup, the inventory
                // item behind it names the actual tiered item.
                std::uintptr_t invDesc = ResolveItemDescriptionDeep(lootObjectItem);
                int basePrice = 0, sellPrice = 0;
                if (!ReadItemPrices(invDesc, basePrice, sellPrice)) {
                    // The id-validated hop can miss; a description that yields
                    // real prices is equally proof it is the item description.
                    const std::uintptr_t candidate = ReadFuncTarget(
                        lootObjectItem, rr::LootObjectItemDescriptionModel::ItemDescriptionFunc);
                    int candBase = 0, candSell = 0;
                    if (candidate > 0x10000 && ReadItemPrices(candidate, candBase, candSell)) {
                        invDesc = candidate;
                        basePrice = candBase;
                        sellPrice = candSell;
                    }
                }
                // The object actually sitting in the world is the authority on
                // what it is; the item description behind it only supplies tier
                // and rarity. Preferring the description first is what labelled
                // an Eclipse bottle as a Golden Jaguar - one id apart.
                std::string name = ReadIdentifiedId(lootObjectItem);
                if (!IsKnownItemId(name)) {
                    const std::string descId = ReadIdentifiedId(invDesc);
                    if (IsKnownItemId(descId)) name = descId;
                    else if (!LooksLikeId(name)) name = descId;
                }
                if (LooksLikeId(name))
                    out.entities[it->second].label = std::move(name);

                if (invDesc > 0x10000) {
                    const std::string descId = ReadIdentifiedId(invDesc);
                    const std::string shownId = out.entities[it->second].label;
                    if (LenientEquals(descId, shownId)) {
                        const int rarity = RobRunMemory::ReadInt32(
                            invDesc + rr::InventoryItemDescriptionModel::Rarity);
                        if (rarity > 0 && rarity <= 6)
                            out.entities[it->second].rarity = rarity;
                    }
                }

                if (sellPrice > 0) {
                    out.entities[it->second].value = sellPrice;
                    out.entities[it->second].hasValue = true;
                }
                LiveItemInfo regInfo{};
                if (LookupLiveItemInfo(out.entities[it->second].label, regInfo)) {
                    if (!out.entities[it->second].hasValue && regInfo.sellPrice > 0) {
                        out.entities[it->second].value = regInfo.sellPrice;
                        out.entities[it->second].hasValue = true;
                    }
                    if (out.entities[it->second].rarity == 0 && regInfo.rarity > 0)
                        out.entities[it->second].rarity = regInfo.rarity;
                }

                // One sample, so the shipped sell price can be compared with the
                // database's number without guessing which one is the buy price.
                static bool s_loggedPrices = !kProbes;
                if (!s_loggedPrices && (sellPrice > 0 || basePrice > 0)) {
                    s_loggedPrices = true;
                    VeoxDiag("price probe: id='%s' base=%d sell=%d",
                        ReadIdentifiedId(invDesc).c_str(), basePrice, sellPrice);
                }
            }
        }
    }

    // The selected loot description comes from _item.Value on the model.
    {
        std::vector<RobRunMemory::DictEntry> entries;
        if (ReadCollection(world, rr::WorldEntitiesModel::RandomLootObjects, entries)) {
            std::unordered_map<std::int32_t, std::size_t> idx;
            for (std::size_t i = randomLootStart; i < out.entities.size(); ++i)
                idx[out.entities[i].id] = i;
            for (const auto& e : entries) {
                auto it = idx.find(e.key);
                if (it == idx.end()) continue;

                auto& entity = out.entities[it->second];
                const std::uintptr_t wrapper = RobRunMemory::ReadPtr(
                    e.value + rr::RandomLootObjectModel::Item);
                const std::uintptr_t klass = Il2Cpp::ClassOf(wrapper);
                const std::string wrapperClass = Il2Cpp::ClassNameOfClass(klass);
                const bool verifiedLayout = wrapperClass == "ReferenceData`1" &&
                    Il2Cpp::NamespaceOfClass(klass) == "Framework.Replication.Data.Primitive";

                std::uintptr_t value = 0;
                const bool valueRead = verifiedLayout && RobRunMemory::ReadBytes(
                    wrapper + rr::ReferenceData::Value, &value, sizeof(value));
                if (valueRead && Il2Cpp::IsClass(value, "LootObjectItemDescription")) {
                    const std::string lootId = ReadIdentifiedId(value);
                    std::uintptr_t invDesc = ResolveInventoryItemDescription(value);
                    const std::string invId = invDesc > 0x10000 ? ReadIdentifiedId(invDesc) : std::string{};
                    if (IsKnownItemId(invId))
                        entity.label = invId;
                    else if (LooksLikeId(lootId))
                        entity.label = lootId;
                    else if (LooksLikeId(invId))
                        entity.label = invId;

                    bool priced = false;
                    if (invDesc > 0x10000) {
                        int basePrice = 0, sellPrice = 0;
                        if (ReadItemPrices(invDesc, basePrice, sellPrice) && sellPrice > 0) {
                            entity.value = sellPrice;
                            entity.hasValue = true;
                            priced = true;
                        }
                        const int r = RobRunMemory::ReadInt32(
                            invDesc + rr::InventoryItemDescriptionModel::Rarity);
                        if (r > 0 && r <= 6)
                            entity.rarity = r;
                    }
                    LiveItemInfo regInfo{};
                    if (LookupLiveItemInfo(entity.label, regInfo)) {
                        if (!priced && regInfo.sellPrice > 0) {
                            entity.value = regInfo.sellPrice;
                            entity.hasValue = true;
                        }
                        if (entity.rarity == 0 && regInfo.rarity > 0)
                            entity.rarity = regInfo.rarity;
                    }
                }

                if constexpr (kProbes) {
                    static bool s_probedRandom = false;
                    if (!s_probedRandom) {
                        s_probedRandom = true;
                        VeoxDiag("probeRandom: placement=%d model=%llX wrapper=%llX class=%s valueRead=%d id='%s'",
                            e.key, (unsigned long long)e.value, (unsigned long long)wrapper,
                            wrapperClass.c_str(), valueRead, entity.label.c_str());
                        ProbeWords("  random.item", wrapper, 4);
                    }
                }
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

    // Loot node type lives behind LocationLootNodeDescription.LootNodeDescriptionFunc;
    // the description object itself only holds an entity id, not a name.
    {
        std::vector<RobRunMemory::DictEntry> entries;
        if (ReadCollection(world, rr::WorldEntitiesModel::LootNodes, entries)) {
            std::unordered_map<std::int32_t, std::size_t> idx;
            for (std::size_t i = 0; i < out.entities.size(); ++i)
                if (out.entities[i].kind == Kind::LootNode)
                    idx[out.entities[i].id] = i;
            for (const auto& e : entries) {
                auto it = idx.find(e.key);
                if (it == idx.end() || e.value <= 0x10000) continue;
                const std::uintptr_t desc = RobRunMemory::ReadPtr(
                    e.value + rr::LootNodeModel::Description);
                const std::uintptr_t nodeDesc = ReadFuncIdentifiedTarget(
                    desc, rr::LocationLootNodeDescriptionModel::LootNodeDescriptionFunc);
                const std::string id = ReadIdentifiedId(nodeDesc);
                if (LooksLikeId(id))
                    out.entities[it->second].label = id;
            }
        }
    }

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

    // Contents of vehicle trunks and plot containers/tables, so an item can be
    // shown in place instead of only when it is lying on the ground.
    if (ConfigInstance.LootFilter.ReadVehicleInventories) {
        AddVehicleTrunkContents(world, out.entities);
        AddPlotContainerContents(world, out.entities);
    }
    // Trunk and table contents carry no position of their own: they belong to the
    // vehicle or container whose dictionary id they share.
    for (auto& entry : out.entities) {
        if (!entry.inVehicle && !entry.inTable) continue;
        for (const auto& parent : out.entities) {
            if (parent.id != entry.id) continue;
            if (entry.inVehicle && parent.kind != Kind::Car && parent.kind != Kind::Helicopter) continue;
            if (entry.inTable && parent.kind != Kind::Container && parent.kind != Kind::PlotContainer) continue;
            entry.position = parent.position;
            break;
        }
    }


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

    // Cargo census, once per session: which dictionary actually holds cargo.
    // 0x518 keeps answering "read failed", so walk every cargo-shaped field the
    // dump exposes and report the one that returns entries.
    {
        static bool s_censused = false;
        if (!s_censused) {
            s_censused = true;
            struct Candidate { const char* name; std::uintptr_t offset; };
            const Candidate candidates[] = {
                { "Cargos 0x518",              0x518 },
                { "_cargos 0x0C8",             0x0C8 },
                { "BuildingCargos 0x558",      0x558 },
                { "_buildingCargos 0x100",     0x100 },
                { "CollectionCargos 0x520",    0x520 },
                { "CargoSpawnPoints 0x260",    0x260 },
            };
            for (const auto& c : candidates) {
                std::vector<RobRunMemory::DictEntry> probe;
                const bool ok = ReadCollection(world, c.offset, probe);
                if (kProbes) VeoxDiag("cargo census %-22s raw=%llX read=%d entries=%zu",
                    c.name, (unsigned long long)RobRunMemory::ReadPtr(world + c.offset),
                    ok ? 1 : 0, probe.size());
            }
        }
    }

    // Cargo lives in BuildingCargos (0x558): the loose Cargos dictionary reads
    // as empty on this build while this one carries every cargo on the map.
    // BuildingStaticCargoModel: description at 0x18, which holds the inline
    // position (0x24) and a Func<CargoDescription> at 0x30 for the cargo type.
    {
        std::vector<RobRunMemory::DictEntry> entries;
        if (ReadCollection(world, rr::WorldEntitiesModel::BuildingCargos, entries)) {
            for (const auto& e : entries) {
                const std::uintptr_t model = e.value;
                if (model <= 0x10000) continue;

                const std::uintptr_t desc = RobRunMemory::ReadPtr(
                    model + rr::BuildingCargoModel::Description);
                if (desc <= 0x10000) continue;

                static bool s_probedBuildingCargo = !kProbes;
                if (!s_probedBuildingCargo) {
                    s_probedBuildingCargo = true;
                    if (kProbes) VeoxDiag("probeBuildingCargo: model=%llX desc=%llX",
                        (unsigned long long)model, (unsigned long long)desc);
                    ProbeWords("  bc.model", model, 6);
                    ProbeWords("  bc.desc", desc, 10);
                    const std::uintptr_t fn = RobRunMemory::ReadPtr(
                        desc + rr::LocationStaticCargoDescriptionModel::CargoDescriptionFunc);
                    VeoxDiag("  bc.func0x30=%llX", (unsigned long long)fn);
                    if (fn > 0x10000) {
                        const std::uintptr_t target = RobRunMemory::ReadPtr(fn + 0x20);   // delegate target, not method_ptr
                        ProbeWords("  bc.funcTarget", target, 4);
                    }
                    ProbeWords("  bc.data0x10", RobRunMemory::ReadPtr(model + 0x10), 8);
                }

                Vector3 position{};
                if (!RobRunMemory::ReadVector3(
                        desc + rr::LocationStaticCargoDescriptionModel::Position, position) ||
                    !ValidPosition(position) ||
                    (position.x == 0.f && position.y == 0.f && position.z == 0.f))
                    continue;

                Entity entity{};
                entity.id = e.key;
                entity.kind = Kind::Cargo;
                entity.position = position;

                const std::uintptr_t cargoDesc = ReadFuncIdentifiedTarget(
                    desc, rr::LocationStaticCargoDescriptionModel::CargoDescriptionFunc);
                std::uintptr_t typeDesc = cargoDesc;
                std::string id = ReadIdentifiedId(cargoDesc);

                // Random cargo keeps its type in RandomCargoData (a
                // ReferenceData<CargoDescription>) at model+0x10 rather than
                // behind the description func. Probe the reference slots and
                // accept the first that reads back a real identifier.
                if (!LooksLikeId(id)) {
                    const std::uintptr_t data = RobRunMemory::ReadPtr(model + 0x10);
                    if (data > 0x10000) {
                        for (const std::uintptr_t off : { (std::uintptr_t)0x10, (std::uintptr_t)0x18,
                                                          (std::uintptr_t)0x20, (std::uintptr_t)0x28 }) {
                            const std::uintptr_t candidate = RobRunMemory::ReadPtr(data + off);
                            const std::string candidateId = ReadIdentifiedId(candidate);
                            if (LooksLikeId(candidateId)) { id = candidateId; typeDesc = candidate; break; }
                        }
                    }
                }
                entity.label = LooksLikeId(id) ? id : KindName(Kind::Cargo);

                if (typeDesc > 0x10000) {
                    const int exchange = RobRunMemory::ReadInt32(
                        typeDesc + rr::CargoDescriptionModel::ExchangeCoinsReward);
                    const int arrest = RobRunMemory::ReadInt32(
                        typeDesc + rr::CargoDescriptionModel::ArrestCoinsReward);
                    const int value = exchange > 0 ? exchange : arrest;
                    if (value > 0) { entity.value = value; entity.hasValue = true; }
                    const int rarity = RobRunMemory::ReadInt32(
                        typeDesc + rr::CargoDescriptionModel::Rare);
                    if (rarity > 0 && rarity <= 6) entity.rarity = rarity;
                }

                out.entities.push_back(std::move(entity));
                ++out.loots;
                if (out.entities.size() >= 4096) break;
            }
        }
    }

    // Cargo: the ReadOnly view at 0x518 refuses to open on some builds, so the
    // plain instance dictionary at 0xC8 is a second chance. Pick whichever
    // yields entries and use it for both the position pass and the labels.
    std::uintptr_t cargoOffset = rr::WorldEntitiesModel::Cargos;
    {
        std::vector<RobRunMemory::DictEntry> probe;
        if (!ReadCollection(world, cargoOffset, probe) || probe.empty()) {
            std::vector<RobRunMemory::DictEntry> alt;
            if (ReadCollection(world, rr::WorldEntitiesModel::CargosInstance, alt) && !alt.empty())
                cargoOffset = rr::WorldEntitiesModel::CargosInstance;
        }
        static std::uintptr_t s_loggedOffset = 0;
        if (s_loggedOffset != cargoOffset) {
            s_loggedOffset = cargoOffset;
            if (kProbes) VeoxDiag("cargo: using offset 0x%llX", (unsigned long long)cargoOffset);
        }
    }

    AddDictionary(world, cargoOffset, Kind::Cargo,
        [](std::uintptr_t model, Vector3& position) {
            const std::uintptr_t transform = RobRunMemory::ReadPtr(
                model + rr::CargoModel::Transform);
            if (ReadVehiclePosition(transform, position)) return true;
            return ReadInlineField(model, rr::CargoModel::InteractPosition, position);
        }, out, out.loots);

    // Cargo carries its type on the description object (CargoDescription ->
    // EnumItem -> Identified). That id is what the loot filter matches an icon
    // and a name on, so replace the generic "Cargo" label with it.
    {
        std::vector<RobRunMemory::DictEntry> entries;
        const bool read = ReadCollection(world, cargoOffset, entries);
        std::size_t matched = 0;
        if (read) {
            std::unordered_map<std::int32_t, std::size_t> idx;
            for (std::size_t i = 0; i < out.entities.size(); ++i)
                if (out.entities[i].kind == Kind::Cargo)
                    idx[out.entities[i].id] = i;
            matched = idx.size();
            for (const auto& e : entries) {
                auto it = idx.find(e.key);
                if (it == idx.end() || e.value <= 0x10000) continue;
                const std::uintptr_t desc = RobRunMemory::ReadPtr(
                    e.value + rr::CargoModel::Description);
                std::string id = ReadIdentifiedId(desc);
                if (!id.empty())
                    out.entities[it->second].label = std::move(id);

                // The description also holds the game's coin values for this
                // cargo type: exchange first (its worth), arrest as a fallback.
                if (desc > 0x10000) {
                    const int exchange = RobRunMemory::ReadInt32(
                        desc + rr::CargoDescriptionModel::ExchangeCoinsReward);
                    const int arrest = RobRunMemory::ReadInt32(
                        desc + rr::CargoDescriptionModel::ArrestCoinsReward);
                    const int value = exchange > 0 ? exchange : arrest;
                    if (value > 0) {
                        out.entities[it->second].value = value;
                        out.entities[it->second].hasValue = true;
                    }
                    const int rarity = RobRunMemory::ReadInt32(
                        desc + rr::CargoDescriptionModel::Rare);
                    if (rarity > 0 && rarity <= 6)
                        out.entities[it->second].rarity = rarity;
                }
            }
        }

        // Cargo is the one kind that has never drawn. Log the read result so the
        // next field report says whether the dictionary is empty or every entry
        // is being dropped for a bad position.
        static ULONGLONG s_lastCargoLog = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - s_lastCargoLog > 5000) {
            s_lastCargoLog = now;
            if (!read)
                if (kProbes) VeoxDiag("cargo: dictionary read FAILED (offset 0x%llX)",
                    (unsigned long long)rr::WorldEntitiesModel::Cargos);
            else
                if (kProbes) VeoxDiag("cargo: entries=%zu placed=%zu", entries.size(), matched);

            static bool s_probedCargo = !kProbes;
            if (!s_probedCargo) {
                s_probedCargo = true;
                const std::uintptr_t field = RobRunMemory::ReadPtr(
                    world + rr::WorldEntitiesModel::Cargos);
                if (kProbes) VeoxDiag("probeCargo: world=%llX field(+0x518)=%llX",
                    (unsigned long long)world, (unsigned long long)field);
                ProbeWords("  cargo.field", field, 6);
                ProbeWords("  cargo.count", RobRunMemory::ReadPtr(field + rr::Dict::count), 2);
            }
        }
    }

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
                entity.label = LooksLikeId(name) ? std::move(name) : std::string("Cash Node");
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
