#include "pch.h"
#include "SkinChanger.h"
#include "RobRunReader.h"
#include "RobRunSDK.h"
#include "RobRunMemory.h"
#include "Il2Cpp.h"
#include "../Config/ConfigInstance.h"
#include "../Memory/Memory.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cctype>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

extern Memory TargetProcess;

namespace SkinChanger {

static_assert(kSlotCount == SkinConfig::kSlots, "SkinConfig slot count");

// ---------------------------------------------------------------------------
// Offsets. Snapshot / loading model / customizer come from the unknowncheats
// posts #1612 / #1676 (after the 2026-11-20 update); the view description
// layout was confirmed with a field dump of the live game (2026-11-21).
// Per-slot fields are resolved by name; the numbers are fallbacks.
// ---------------------------------------------------------------------------
namespace off {
inline constexpr std::uintptr_t ClientMaleLoading   = 0xE8;  // <MaleSkinLoadingModel>
inline constexpr std::uintptr_t ClientFemaleLoading = 0xF0;  // <FemaleSkinLoadingModel>
inline constexpr std::uintptr_t LoadingVersion      = 0x18;  // int <LoadingVersion>
inline constexpr std::uintptr_t LoadingCurrent      = 0x28;  // <CurrentValue> snapshot
inline constexpr std::uintptr_t SnapCustomizer      = 0x10;  // _customizerViewDescription
inline constexpr std::uintptr_t SnapVersion         = 0x18;  // int _version

// ViewDescriptions`2
inline constexpr std::uintptr_t ViewDescsList       = 0x18;  // <List> (menu order)
inline constexpr std::uintptr_t ViewDescsDictionary = 0x20;  // _dictionary (lookup)
inline constexpr std::uintptr_t ViewItemIdentified  = 0x10;  // <Identified> -> _id
inline constexpr std::uintptr_t ViewItemName        = 0x30;  // <Name> LocalizedString
inline constexpr std::uintptr_t LocalizedText       = 0xA0;  // m_CurrentStringChangedValue
inline constexpr std::uintptr_t IdentifiedId        = 0x10;  // id string (descriptions too)

// Il2Cpp List<T>
inline constexpr std::uintptr_t ListItems           = 0x10;
inline constexpr std::uintptr_t ListCount           = 0x18;
inline constexpr std::uintptr_t ArrayData           = 0x20;

// BattleEntitiesUpdater -> local character view -> customization view
inline constexpr std::uintptr_t ViewCustomization       = 0xB8;
inline constexpr std::uintptr_t ViewBattleCustomization = 0xC0;
inline constexpr std::uintptr_t BattleCustView          = 0x20;
inline constexpr std::uintptr_t CustViewIsCustomized    = 0x128; // <IsCustomized>
} // namespace off

// Per slot: the snapshot field holding the real item and the customizer
// property holding its view descriptions, female / male. Female names and
// offsets are from the dump; male names follow the same pattern.
struct SlotDef {
    const char* name;
    const char* category;          // SkinNames.inc category
    const char* snapFemale;
    const char* snapMale;
    std::uintptr_t snapFemaleFallback;
    std::uintptr_t snapMaleFallback;
    const char* viewsFemale;
    const char* viewsMale;
    std::uintptr_t viewsFemaleFallback;
    std::uintptr_t viewsMaleFallback;
    const char* visibleField;      // snapshot bool, nullptr if none
};

static const SlotDef kSlots[kSlotCount] = {
    { "Outfit", "outfit", "_fBody", "_body", 0x20, 0x28,
      "<FBodiesViewDescription>k__BackingField", "<BodiesViewDescription>k__BackingField",
      0x88, 0x20, nullptr },
    { "Mask", "mask", "_fMask", "_mask", 0x60, 0,
      "<FMasksViewDescription>k__BackingField", "<MasksViewDescription>k__BackingField",
      0x48, 0x40, "<IsMaskVisible>k__BackingField" },
    { "Bag", "bag", "_fBag", "_bag", 0x58, 0,
      "<BagsViewDescription>k__BackingField", "<BagsViewDescription>k__BackingField",
      0x38, 0x38, "<IsBagVisible>k__BackingField" },
    { "Keychain", "keychain", "_fKeychain", "_keychain", 0x90, 0,
      "<KeychainsViewDescription>k__BackingField", "<KeychainsViewDescription>k__BackingField",
      0x68, 0x68, nullptr },
};

const char* SlotName(int slot)
{
    return (slot >= 0 && slot < kSlotCount) ? kSlots[slot].name : "?";
}

// ---------------------------------------------------------------------------
// Readable names (SkinNames.inc). Game ids ("f_body_2_type_1_paint_1") and
// catalog ids ("Body2Type1Paint1") are compared without case, underscores and
// the female "f" prefix.
// ---------------------------------------------------------------------------
struct SkinName { const char* category; const char* id; const char* name; };
static const SkinName kSkinNames[] = {
#include "SkinNames.inc"
};

static bool SameId(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

static std::string NameKey(const std::string& id)
{
    std::string s;
    for (unsigned char c : id)
        if (c != '_') s.push_back(static_cast<char>(std::tolower(c)));
    for (const char* word : { "body", "mask", "bag", "keychain", "hair", "head" }) {
        const std::size_t n = std::char_traits<char>::length(word);
        if (s.size() > n && s[0] == 'f' && s.compare(1, n, word) == 0) {
            s.erase(0, 1);
            break;
        }
    }
    return s;
}

std::string Label(int slot, const std::string& id)
{
    if (id.empty()) return "-";
    const char* category = (slot >= 0 && slot < kSlotCount) ? kSlots[slot].category : "";
    const std::string key = NameKey(id);
    for (const auto& n : kSkinNames)
        if (std::strcmp(n.category, category) == 0 && NameKey(n.id) == key)
            return n.name;
    return id;
}

// ---------------------------------------------------------------------------
// Memory helpers
// ---------------------------------------------------------------------------
static bool Valid(std::uintptr_t p) { return p >= 0x10000 && p < 0x7FFFFFFFFFFFull; }

static std::uintptr_t Ptr(std::uintptr_t address)
{
    return Valid(address) ? RobRunMemory::ReadPtr(address) : 0;
}

// Uncached read for values that change at runtime and for read-back after a
// write: the cached path can return copies a few hundred ms old.
static std::uintptr_t PtrFresh(std::uintptr_t address)
{
    std::uintptr_t v = 0;
    return (Valid(address) && RobRunMemory::ReadBytesFresh(address, &v, sizeof(v))) ? v : 0;
}

static bool Is(std::uintptr_t object, const char* cls)
{
    return Valid(object) && Il2Cpp::IsClass(object, cls);
}

static bool ClassContains(std::uintptr_t object, const char* needle)
{
    if (!Valid(object)) return false;
    return Il2Cpp::ClassNameOf(object).find(needle) != std::string::npos;
}

static void BumpInt(std::uintptr_t address)
{
    std::int32_t v = 0;
    if (!Valid(address) || !RobRunMemory::ReadBytesFresh(address, &v, sizeof(v))) return;
    v = (v == INT_MAX) ? 0 : v + 1;
    TargetProcess.Write(address, v);
}

static std::string ReadId(std::uintptr_t stringObject)
{
    std::wstring w;
    if (!Valid(stringObject) || !RobRunMemory::ReadString(stringObject, w, 128))
        return {};
    return RobRunMemory::WideToUtf8(w);
}

// Id of a description / IdentifiedProperty / string key. Cached: these are
// static catalog objects that live for the whole game process.
static std::string IdOf(std::uintptr_t object)
{
    static std::unordered_map<std::uintptr_t, std::string> cache;
    if (!Valid(object)) return {};
    auto it = cache.find(object);
    if (it != cache.end()) return it->second;
    std::string id = Il2Cpp::ClassNameOf(object) == "String"
        ? ReadId(object) : ReadId(Ptr(object + off::IdentifiedId));
    if (cache.size() > 8192) cache.clear();
    if (!id.empty()) cache.emplace(object, id);
    return id;
}

// Field offset by name on the object's class (with parents), cached per
// class; fallback when the field is not found.
static std::uintptr_t FieldOffset(std::uintptr_t object, const char* name, std::uintptr_t fallback)
{
    static std::unordered_map<std::string, std::int32_t> cache;
    if (!Valid(object) || !name) return fallback;
    const std::uintptr_t klass = Il2Cpp::ClassOf(object);
    if (!klass) return fallback;
    const std::string key = std::to_string(klass) + ':' + name;
    auto it = cache.find(key);
    if (it == cache.end()) {
        Il2Cpp::Field field{};
        const bool found = Il2Cpp::FindField(klass, name, field, true) &&
            field.offset > 0 && field.offset < 0x1000;
        it = cache.emplace(key, found ? field.offset : -1).first;
    }
    return it->second > 0 ? static_cast<std::uintptr_t>(it->second) : fallback;
}

// ---------------------------------------------------------------------------
// Context: the local player's objects, re-resolved every second.
// ---------------------------------------------------------------------------
struct SlotContext {
    std::uintptr_t snapOffset = 0;     // real item description in the snapshot
    std::uintptr_t views = 0;          // ViewDescriptions`2 object
    std::uintptr_t visibleOffset = 0;  // snapshot bool, 0 if none
};

struct Context {
    bool valid = false;
    bool female = false;
    std::int32_t localId = -1;
    std::uintptr_t loadingModel = 0;
    std::uintptr_t snapshot = 0;
    std::uintptr_t customizer = 0;
    std::uintptr_t updater = 0;
    std::uintptr_t client = 0;
    std::uintptr_t serverModel = 0;    // BattleCharacterModel (<CurrentWeaponSkin>)
    std::uintptr_t game = 0;
    SlotContext slots[kSlotCount];
    std::uintptr_t weaponViews = 0;     // BattleWeaponSkinViewDescriptions (battle lookup)
    std::uintptr_t weaponNameViews = 0; // WeaponSkinViewDescriptions (skin center, names)
};

static const char* ResolveContext(const RobRunReader::Snapshot& snap, Context& ctx)
{
    ctx = Context{};
    ctx.updater = snap.battleEntitiesUpdater;
    ctx.localId = snap.localEntityId;
    ctx.game = snap.game;
    if (!Is(snap.battleEntities, "BattleEntitiesModel"))
        return "Not in a match";

    const std::uintptr_t playerEnt = Ptr(snap.battleEntities + rr::BattleEntitiesModel::LocalPlayerEntityModel);
    if (!Is(playerEnt, "PlayerEntityModel"))
        return "Local player not found";
    const std::uintptr_t client = Ptr(playerEnt + rr::PlayerEntityModel::Client);
    ctx.client = client;
    ctx.serverModel = Ptr(playerEnt + rr::PlayerEntityModel::Server);
    if (!Valid(client))
        return "Local client model not found";

    // Gender = whichever loading model holds a snapshot of the right class.
    const std::uintptr_t femaleLoading = Ptr(client + off::ClientFemaleLoading);
    const std::uintptr_t maleLoading = Ptr(client + off::ClientMaleLoading);
    const std::uintptr_t femaleSnap = Ptr(femaleLoading + off::LoadingCurrent);
    const std::uintptr_t maleSnap = Ptr(maleLoading + off::LoadingCurrent);
    if (Is(femaleSnap, "CustomizationFemaleSnapshotModel")) {
        ctx.female = true;
        ctx.loadingModel = femaleLoading;
        ctx.snapshot = femaleSnap;
    } else if (Is(maleSnap, "CustomizationMaleSnapshotModel")) {
        ctx.loadingModel = maleLoading;
        ctx.snapshot = maleSnap;
    } else {
        return "Skin snapshot not found (offsets changed?)";
    }

    ctx.customizer = Ptr(ctx.snapshot + off::SnapCustomizer);
    if (!Is(ctx.customizer, "CustomizerViewDescription"))
        return "Skin view data not found (offsets changed?)";

    for (int i = 0; i < kSlotCount; ++i) {
        const SlotDef& d = kSlots[i];
        SlotContext& s = ctx.slots[i];
        s.snapOffset = FieldOffset(ctx.snapshot, ctx.female ? d.snapFemale : d.snapMale,
            ctx.female ? d.snapFemaleFallback : d.snapMaleFallback);
        const std::uintptr_t viewsOffset = FieldOffset(ctx.customizer,
            ctx.female ? d.viewsFemale : d.viewsMale,
            ctx.female ? d.viewsFemaleFallback : d.viewsMaleFallback);
        s.views = viewsOffset ? Ptr(ctx.customizer + viewsOffset) : 0;
        if (!ClassContains(s.views, "ViewDescription")) s.views = 0;
        s.visibleOffset = d.visibleField ? FieldOffset(ctx.snapshot, d.visibleField, 0) : 0;
    }

    // Weapon skin lookups, by field name (optional: cosmetics work without).
    if (Is(ctx.updater, "BattleEntitiesUpdater")) {
        const std::uintptr_t contentOffset = FieldOffset(ctx.updater, "_battleScreenContent", 0);
        const std::uintptr_t content = contentOffset ? Ptr(ctx.updater + contentOffset) : 0;
        if (Valid(content)) {
            const std::uintptr_t battleOffset = FieldOffset(content,
                "<BattleWeaponSkinViewDescriptions>k__BackingField", 0);
            const std::uintptr_t namesOffset = FieldOffset(content,
                "<WeaponSkinViewDescriptions>k__BackingField", 0);
            if (battleOffset) ctx.weaponViews = Ptr(content + battleOffset);
            if (namesOffset) ctx.weaponNameViews = Ptr(content + namesOffset);
            if (!Is(ctx.weaponViews, "BattleWeaponSkinViewDescriptions")) ctx.weaponViews = 0;
            if (!Is(ctx.weaponNameViews, "WeaponSkinViewDescriptions")) ctx.weaponNameViews = 0;
        }
    }

    ctx.valid = true;
    return nullptr;
}

// The game's own display name of a view item: its <Name> LocalizedString
// keeps the last translated text (m_CurrentStringChangedValue). Empty when
// the game has not translated that item yet.
static std::string LocalizedName(std::uintptr_t viewItem)
{
    const std::uintptr_t localized = Ptr(viewItem +
        FieldOffset(viewItem, "<Name>k__BackingField", off::ViewItemName));
    if (!ClassContains(localized, "LocalizedString")) return {};
    return ReadId(Ptr(localized +
        FieldOffset(localized, "m_CurrentStringChangedValue", off::LocalizedText)));
}

struct Option { std::string id, name; };

// Item ids (and game names) in menu order (ViewDescriptions <List>).
static std::vector<Option> ReadOptions(std::uintptr_t views)
{
    std::vector<Option> options;
    const std::uintptr_t list = Ptr(views + off::ViewDescsList);
    const std::uintptr_t items = Ptr(list + off::ListItems);
    const std::int32_t count = RobRunMemory::ReadInt32(list + off::ListCount);
    if (!Valid(items) || count <= 0 || count > 4096) return options;
    for (std::int32_t i = 0; i < count; ++i) {
        const std::uintptr_t item = Ptr(items + off::ArrayData +
            static_cast<std::uintptr_t>(i) * sizeof(std::uintptr_t));
        std::string id = IdOf(Ptr(item + off::ViewItemIdentified));
        if (!id.empty()) options.push_back(Option{ std::move(id), LocalizedName(item) });
    }
    return options;
}

// One entry of a lookup dictionary (key = item, value = its view description).
struct DictSlot {
    std::uintptr_t valueAddress = 0;
    std::uintptr_t value = 0;
};

static bool FindSlots(std::uintptr_t dict, const std::string& idA, DictSlot& a,
    const std::string& idB, DictSlot& b)
{
    a = DictSlot{};
    b = DictSlot{};
    if (!ClassContains(dict, "Dictionary")) return false;
    const std::uintptr_t entries = Ptr(dict + rr::Dict::entries);
    const std::int32_t count = RobRunMemory::ReadInt32(dict + rr::Dict::count);
    if (!Valid(entries) || count <= 0 || count > 8192) return false;
    for (std::int32_t i = 0; i < count; ++i) {
        const std::uintptr_t entry = entries + rr::Array::vector +
            static_cast<std::uintptr_t>(i) * rr::Dict::entry_stride;
        if (RobRunMemory::ReadInt32(entry + rr::Dict::entry_next) < rr::Dict::entry_allocated_min)
            continue;   // free slot
        const std::string id = IdOf(Ptr(entry + rr::Dict::entry_key));
        if (id.empty()) continue;
        DictSlot* slot = SameId(id, idA) ? &a : SameId(id, idB) ? &b : nullptr;
        if (!slot) continue;
        slot->valueAddress = entry + rr::Dict::entry_value;
        slot->value = PtrFresh(slot->valueAddress);
    }
    return a.valueAddress && b.valueAddress;
}

// Makes the local character rebuild its look: bump the snapshot and loading
// versions and mark the view as not customized. All local (not replicated).
static void RebuildLocalView(const Context& ctx)
{
    BumpInt(ctx.snapshot + off::SnapVersion);
    BumpInt(ctx.loadingModel + off::LoadingVersion);
    std::vector<RobRunMemory::DictEntry> views;
    const std::uintptr_t viewDict = Ptr(ctx.updater + rr::BattleEntitiesUpdater::_characterEntityViews);
    if (!Is(ctx.updater, "BattleEntitiesUpdater") || ctx.localId < 0 ||
        !RobRunMemory::ReadDictInt(viewDict, views, false))
        return;
    for (const auto& v : views) {
        if (v.key != ctx.localId || !Valid(v.value)) continue;
        std::uintptr_t custView = Ptr(v.value + off::ViewCustomization);
        if (!Is(custView, "CharacterCustomizationView"))
            custView = Ptr(Ptr(v.value + off::ViewBattleCustomization) + off::BattleCustView);
        if (Is(custView, "CharacterCustomizationView")) {
            const std::uint8_t notCustomized = 0;
            TargetProcess.Write(custView + off::CustViewIsCustomized, notCustomized);
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// Swaps: per slot, the lookup entry of the real item points at the chosen
// item's view description. Restored whenever no longer wanted and on Stop.
// ---------------------------------------------------------------------------
struct Swap {
    bool active = false;
    std::uintptr_t dict = 0;
    std::uintptr_t valueAddress = 0;
    std::uintptr_t original = 0;   // real item's own view description
    std::uintptr_t applied = 0;    // chosen item's view description
    std::string realId, wantedId;
};
static Swap g_swaps[kSlotCount];

static bool RestoreSwap(Swap& swap)
{
    if (!swap.active) return false;
    // Only put the original back if the entry still holds our value.
    if (PtrFresh(swap.valueAddress) == swap.applied)
        TargetProcess.Write(swap.valueAddress, swap.original);
    swap = Swap{};
    return true;
}

static void RestoreAll()
{
    for (auto& swap : g_swaps) RestoreSwap(swap);
}

// Returns nullptr on success, otherwise the reason. changed = entry written.
static const char* EnsureSwap(Swap& swap, std::uintptr_t views, const std::string& realId,
    const std::string& wanted, bool& changed, bool& redone)
{
    changed = false;
    redone = false;
    const std::uintptr_t dict = Ptr(views + off::ViewDescsDictionary);

    if (swap.active) {
        const bool same = swap.dict == dict && SameId(swap.realId, realId) &&
            SameId(swap.wantedId, wanted);
        if (same && PtrFresh(swap.valueAddress) == swap.applied)
            return nullptr;                 // still in place
        if (same) {
            swap = Swap{};                  // game rebuilt the entry: redo it
            redone = true;
        } else {
            RestoreSwap(swap);              // something changed: undo first
        }
    }

    DictSlot real, target;
    if (!FindSlots(dict, realId, real, wanted, target)) {
        if (!real.valueAddress) return "Your item is not in the lookup";
        return "Chosen item is not in the lookup";
    }
    if (!ClassContains(target.value, "ViewDescription") ||
        !ClassContains(real.value, "ViewDescription"))
        return "Lookup looks wrong (offsets changed?)";

    if (!TargetProcess.Write(real.valueAddress, target.value) ||
        PtrFresh(real.valueAddress) != target.value)
        return "Write failed";

    swap.active = true;
    swap.dict = dict;
    swap.valueAddress = real.valueAddress;
    swap.original = real.value;
    swap.applied = target.value;
    swap.realId = realId;
    swap.wantedId = wanted;
    changed = true;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Weapon skins. Lookup: BattleEntitiesUpdater._battleScreenContent
//   .<BattleWeaponSkinViewDescriptions> (WeaponSkinDescription ->
//   BattleWeaponSkinViewDescription), found with a reference search of the
//   live game (2026-11-21). Skin ids are "<weapon>_s_<n>", e.g. "rc_556_s_5".
// For a weapon with a chosen skin, every skin entry of that weapon is pointed
// at the chosen skin's view description, so the weapon shows the chosen skin
// whichever skin is really equipped (also on other players, on this screen).
// ---------------------------------------------------------------------------
struct WeaponName { const char* key; const char* name; };
static const WeaponName kWeaponNames[] = {
    { "jp7", "JP-7" }, { "dp9", "DP-9" }, { "kl545", "KL-545" }, { "lmg", "M249 LMG" },
    { "cb45", "CB45" }, { "harris500", "Harris 500" }, { "rc556", "RC 5.56" },
    { "starh", "Star-H" }, { "srreaper", "SR Reaper" }, { "pdw90", "PDW90" },
    { "apscalar", "AP Scalar" }, { "ordr12", "ORDR-12" }, { "lbr44", "LBR44" },
    { "tag12k", "TAG-12k" }, { "drm14", "DRM-14" }, { "mc10", "MC-10" },
    { "vulture", "Vulture 50" },
};

static std::string WeaponKeyOf(const std::string& skinId)
{
    const std::size_t pos = skinId.rfind("_s_");
    return pos == std::string::npos ? skinId : skinId.substr(0, pos);
}

static std::string WeaponDisplayName(const std::string& key)
{
    const std::string norm = NameKey(key);
    for (const auto& w : kWeaponNames)
        if (norm == w.key) return w.name;
    std::string s = key;
    for (char& c : s) c = c == '_' ? ' ' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Catalog name of a weapon skin without the weapon prefix ("Factory Paint").
static std::string WeaponSkinCatalogName(const std::string& skinId)
{
    const std::string key = NameKey(skinId);
    for (const auto& n : kSkinNames) {
        if (std::strcmp(n.category, "weapon") != 0 || NameKey(n.id) != key) continue;
        std::string label = n.name;
        const std::string weapon = WeaponDisplayName(WeaponKeyOf(skinId)) + " ";
        if (label.compare(0, weapon.size(), weapon) == 0) label.erase(0, weapon.size());
        return label;
    }
    return skinId;
}

struct DictItem { std::string id; std::uintptr_t valueAddress = 0; };

// All used entries of a lookup dictionary with their ids.
static std::vector<DictItem> ReadDictItems(std::uintptr_t dict)
{
    std::vector<DictItem> items;
    if (!ClassContains(dict, "Dictionary")) return items;
    const std::uintptr_t entries = Ptr(dict + rr::Dict::entries);
    const std::int32_t count = RobRunMemory::ReadInt32(dict + rr::Dict::count);
    if (!Valid(entries) || count <= 0 || count > 8192) return items;
    for (std::int32_t i = 0; i < count; ++i) {
        const std::uintptr_t entry = entries + rr::Array::vector +
            static_cast<std::uintptr_t>(i) * rr::Dict::entry_stride;
        if (RobRunMemory::ReadInt32(entry + rr::Dict::entry_next) < rr::Dict::entry_allocated_min)
            continue;
        std::string id = IdOf(Ptr(entry + rr::Dict::entry_key));
        if (!id.empty()) items.push_back(DictItem{ std::move(id), entry + rr::Dict::entry_value });
    }
    return items;
}

struct WeaponSwap {
    bool active = false;
    std::uintptr_t dict = 0;
    std::string chosen;
    std::uintptr_t applied = 0;    // chosen skin's view description
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> entries;   // address, original
    std::uint64_t lastCheck = 0;
    int resets = 0;
};
static std::unordered_map<std::string, WeaponSwap> g_weaponSwaps;

static bool RestoreWeaponSwap(WeaponSwap& swap)
{
    if (!swap.active) return false;
    for (const auto& [address, original] : swap.entries)
        if (PtrFresh(address) == swap.applied)
            TargetProcess.Write(address, original);
    swap = WeaponSwap{};
    return true;
}

static void RestoreWeapons()
{
    for (auto& [key, swap] : g_weaponSwaps) RestoreWeaponSwap(swap);
}

// Points every skin of the weapon at the chosen skin. nullptr on success.
static const char* EnsureWeaponSwap(WeaponSwap& swap, std::uintptr_t dict,
    const std::vector<DictItem>& items, const std::string& key, const std::string& chosen,
    std::uint64_t now)
{
    if (swap.active && swap.dict == dict && SameId(swap.chosen, chosen)) {
        if (now - swap.lastCheck >= 1000) {          // watchdog, uncached
            swap.lastCheck = now;
            bool reset = false;
            for (const auto& [address, original] : swap.entries)
                if (PtrFresh(address) != swap.applied) {
                    TargetProcess.Write(address, swap.applied);
                    reset = true;
                }
            if (reset) ++swap.resets;
        }
        return nullptr;
    }
    RestoreWeaponSwap(swap);

    std::uintptr_t target = 0;
    for (const auto& item : items)
        if (SameId(item.id, chosen)) target = PtrFresh(item.valueAddress);
    if (!ClassContains(target, "ViewDescription"))
        return "Chosen skin is not in the lookup";

    WeaponSwap next;
    next.dict = dict;
    next.chosen = chosen;
    next.applied = target;
    for (const auto& item : items) {
        if (WeaponKeyOf(item.id) != key || SameId(item.id, chosen)) continue;
        const std::uintptr_t original = PtrFresh(item.valueAddress);
        if (original == target || !ClassContains(original, "ViewDescription")) continue;
        if (TargetProcess.Write(item.valueAddress, target))
            next.entries.emplace_back(item.valueAddress, original);
    }
    next.active = true;
    next.lastCheck = now;
    swap = std::move(next);
    return nullptr;
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------
static std::mutex g_stateMutex;
static State g_state;
static std::atomic<bool> g_stop{ false };
static std::thread g_thread;
static std::mutex g_wantedMutex;

static void Publish(const State& s)
{
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_state = s;
}

State GetState()
{
    std::lock_guard<std::mutex> lock(g_stateMutex);
    return g_state;
}

std::string Wanted(int slot, bool female)
{
    if (slot < 0 || slot >= kSlotCount) return {};
    std::lock_guard<std::mutex> lock(g_wantedMutex);
    return female ? ConfigInstance.Skin.Female[slot] : ConfigInstance.Skin.Male[slot];
}

void SetWanted(int slot, bool female, const std::string& id)
{
    if (slot < 0 || slot >= kSlotCount) return;
    std::lock_guard<std::mutex> lock(g_wantedMutex);
    (female ? ConfigInstance.Skin.Female[slot] : ConfigInstance.Skin.Male[slot]) = id;
}

std::string WantedWeaponSkin(const std::string& weaponKey)
{
    std::lock_guard<std::mutex> lock(g_wantedMutex);
    const auto it = ConfigInstance.Skin.WeaponSkins.find(weaponKey);
    return it == ConfigInstance.Skin.WeaponSkins.end() ? std::string() : it->second;
}

void SetWantedWeaponSkin(const std::string& weaponKey, const std::string& skinId)
{
    std::lock_guard<std::mutex> lock(g_wantedMutex);
    if (skinId.empty()) ConfigInstance.Skin.WeaponSkins.erase(weaponKey);
    else ConfigInstance.Skin.WeaponSkins[weaponKey] = skinId;
}

// Weapon part of the worker tick: groups the battle lookup's skins by weapon,
// applies / restores each weapon's chosen skin and reports the held weapon.
static void TickWeapons(const Context& ctx, bool enabled, bool refresh, std::uint64_t now,
    State& state)
{
    static std::uintptr_t dict = 0;
    static std::vector<DictItem> items;
    static std::vector<std::string> order;                          // weapon keys
    static std::unordered_map<std::string, std::vector<std::string>> skins;
    static std::unordered_map<std::string, std::string> names;      // skin id -> name

    const std::uintptr_t liveDict = Ptr(ctx.weaponViews + off::ViewDescsDictionary);
    if (!ctx.weaponViews || !ClassContains(liveDict, "Dictionary")) {
        RestoreWeapons();
        return;
    }
    if (refresh || liveDict != dict || items.empty()) {
        dict = liveDict;
        items = ReadDictItems(dict);
        order.clear();
        skins.clear();
        names.clear();
        // Menu order from the battle list; game names from the skin center list.
        for (const auto& o : ReadOptions(ctx.weaponViews)) {
            const std::string key = WeaponKeyOf(o.id);
            if (!skins.count(key)) order.push_back(key);
            skins[key].push_back(o.id);
        }
        if (ctx.weaponNameViews)
            for (const auto& o : ReadOptions(ctx.weaponNameViews))
                if (!o.name.empty()) names[o.id] = o.name;
    }
    auto skinName = [&](const std::string& id) {
        const auto it = names.find(id);
        return it != names.end() ? it->second : WeaponSkinCatalogName(id);
    };

    state.weaponsSupported = !order.empty();

    // The weapon in hand: character <CurrentWeaponSkin> -> _data -> value.
    const std::uintptr_t prim = Ptr(ctx.serverModel +
        FieldOffset(ctx.serverModel, "<CurrentWeaponSkin>k__BackingField", 0x308));
    const std::string held = IdOf(PtrFresh(Ptr(prim + 0x10) + rr::ReferenceData::Value));
    if (!held.empty()) {
        state.heldWeapon = WeaponDisplayName(WeaponKeyOf(held));
        state.heldSkin = skinName(held);
    }

    for (const std::string& key : order) {
        WeaponState w;
        w.key = key;
        w.name = WeaponDisplayName(key);
        for (const std::string& id : skins[key]) {
            w.skins.push_back(id);
            w.names.push_back(skinName(id));
        }
        WeaponSwap& swap = g_weaponSwaps[key];
        const std::string wanted = WantedWeaponSkin(key);
        if (!enabled || wanted.empty()) {
            RestoreWeaponSwap(swap);
            w.status = !enabled ? "Off" : "Real skins";
        } else if (const char* error = EnsureWeaponSwap(swap, dict, items, key, wanted, now)) {
            w.status = error;
            w.error = true;
        } else {
            w.status = "Showing: " + skinName(wanted) +
                "  (switch weapons once if it does not change right away)";
            if (swap.resets > 0)
                w.status += "  (game reset it " + std::to_string(swap.resets) + "x)";
        }
        state.weapons.push_back(std::move(w));
    }
}

static void Tick()
{
    static Context ctx;
    static std::uint64_t lastResolve = 0, lastOptions = 0;
    static std::uint64_t lastGeneration = 0;
    static std::vector<Option> options[kSlotCount];
    static std::uint64_t lastAttempt[kSlotCount] = {};
    static std::string lastError[kSlotCount];
    static int resets[kSlotCount] = {};   // times the game rebuilt an entry

    const std::uint64_t now = GetTickCount64();
    const auto snap = RobRunReader::GetSnapshot();
    if (!snap) return;

    if (snap->sessionGeneration != lastGeneration) {
        lastGeneration = snap->sessionGeneration;
        ctx = Context{};
        lastResolve = lastOptions = 0;
        for (int i = 0; i < kSlotCount; ++i) {
            options[i].clear();
            lastAttempt[i] = 0;
            lastError[i].clear();
            resets[i] = 0;
        }
    }

    State state;
    if (!ctx.valid || now - lastResolve >= 1000) {
        lastResolve = now;
        const std::uintptr_t before = ctx.customizer;
        const bool beforeFemale = ctx.female;
        if (const char* why = ResolveContext(*snap, ctx)) {
            // Swaps stay while enabled (the lookups outlive a match); turning
            // the changer off restores them even outside a match.
            if (!ConfigInstance.Skin.Enabled) { RestoreAll(); RestoreWeapons(); }
            state.status = why;
            state.error = std::string(why) != "Not in a match";
            Publish(state);
            return;
        }
        if (ctx.customizer != before || ctx.female != beforeFemale) lastOptions = 0;
    }

    const bool refresh = lastOptions == 0 || now - lastOptions >= 5000;
    if (refresh) {
        lastOptions = now;
        for (int i = 0; i < kSlotCount; ++i)
            options[i] = ctx.slots[i].views ? ReadOptions(ctx.slots[i].views) : std::vector<Option>{};
    }

    state.inMatch = true;
    state.female = ctx.female;
    const bool enabled = ConfigInstance.Skin.Enabled;
    bool rebuild = false;

    for (int i = 0; i < kSlotCount; ++i) {
        SlotState& out = state.slots[i];
        const SlotContext& s = ctx.slots[i];
        Swap& swap = g_swaps[i];
        out.supported = s.snapOffset && s.views && !options[i].empty();
        // Display name: the game's localized name, else the catalog, else the id.
        auto niceName = [&](const std::string& id) {
            for (const auto& o : options[i])
                if (!o.name.empty() && SameId(o.id, id)) return o.name;
            return Label(i, id);
        };
        for (const auto& o : options[i]) {
            out.options.push_back(o.id);
            out.names.push_back(o.name.empty() ? Label(i, o.id) : o.name);
        }
        if (!out.supported) {
            if (RestoreSwap(swap)) rebuild = true;
            out.status = "Not available for this character";
            continue;
        }

        // The real item, as the game (and the server) sees it. The snapshot
        // is never written, so this is always the player's own item.
        out.real = IdOf(PtrFresh(ctx.snapshot + s.snapOffset));
        out.realName = out.real.empty() ? std::string("-") : niceName(out.real);
        const std::string wanted = Wanted(i, ctx.female);

        if (!enabled || wanted.empty() || out.real.empty() || SameId(wanted, out.real)) {
            if (RestoreSwap(swap)) rebuild = true;
            out.status = !enabled ? "Off"
                : wanted.empty() ? "Real item"
                : out.real.empty() ? "Waiting for your item"
                : "Already wearing it";
            continue;
        }

        // Re-check every tick (uncached); after an error retry every 1.5 s.
        if (!swap.active && !lastError[i].empty() && now - lastAttempt[i] < 1500) {
            out.status = lastError[i];
            out.error = true;
            continue;
        }
        bool changed = false, redone = false;
        if (const char* error = EnsureSwap(swap, s.views, out.real, wanted, changed, redone)) {
            lastAttempt[i] = now;
            lastError[i] = error;
            out.status = error;
            out.error = true;
            continue;
        }
        lastError[i].clear();
        if (redone) ++resets[i];
        if (changed) rebuild = true;
        out.status = "Showing: " + niceName(wanted);
        if (resets[i] > 0)
            out.status += "  (game reset it " + std::to_string(resets[i]) + "x)";
        if (s.visibleOffset) {
            std::uint8_t visible = 1;
            RobRunMemory::ReadBytesFresh(ctx.snapshot + s.visibleOffset, &visible, 1);
            if (!visible) out.status += "  - hidden: equip one in game to see it";
        }
    }

    if (rebuild) RebuildLocalView(ctx);
    TickWeapons(ctx, enabled, refresh, now, state);
    Publish(state);
}

void Start()
{
    Stop();
    g_stop.store(false);
    g_thread = std::thread([] {
        while (!g_stop.load()) {
            try { Tick(); } catch (...) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        // Never leave the game's lookups swapped behind.
        try { RestoreAll(); RestoreWeapons(); } catch (...) {}
    });
}

void Stop()
{
    g_stop.store(true);
    if (g_thread.joinable()) g_thread.join();
}

} // namespace SkinChanger
