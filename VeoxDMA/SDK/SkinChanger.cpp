#include "pch.h"
#include "SkinChanger.h"
#include "RobRunSDK.h"
#include "RobRunMemory.h"
#include "../Config/ConfigInstance.h"
#include "../Memory/Memory.h"

extern Memory TargetProcess;

namespace SkinChanger {

static std::uintptr_t g_gaBase = 0;

// TODO: Fill these from the game dump once you have the Windows box.
// Each is the TypeInfo RVA for the weapon's skin description singleton class.
// Walk TypeInfo -> klass + 0xB8 -> staticFields -> singleton pointer.
// The offsets in the catalog arrays are field offsets within that singleton.
namespace DescriptionRVA {

// Weapon skin description singletons (one class per weapon group).
// All weapon skins live in a single AllWeaponSkinDescriptions singleton
// whose fields are at the offsets listed in the catalog.
inline constexpr std::uintptr_t WeaponSkins = 0x0; // TODO: TypeInfo RVA

// Cosmetic description singletons (one class per cosmetic category).
inline constexpr std::uintptr_t CharacterBody     = 0x0; // TODO: CharacterBodyDescriptions
inline constexpr std::uintptr_t CharacterFBody    = 0x0; // TODO: CharacterFBodyDescriptions
inline constexpr std::uintptr_t CharacterMask     = 0x0; // TODO: CharacterMaskDescriptions
inline constexpr std::uintptr_t CharacterBag      = 0x0; // TODO: CharacterBagDescriptions
inline constexpr std::uintptr_t CharacterKeychain = 0x0; // TODO: CharacterKeychainDescriptions

inline const std::uintptr_t CosmeticRVAs[kCosmeticCount] = {
    CharacterBody, CharacterFBody, CharacterMask, CharacterBag, CharacterKeychain
};
} // namespace DescriptionRVA

// TODO: These offsets locate the CustomizationLoader and TemporaryCharacter
// at runtime. The reference code accesses them through a chain starting from
// the local player or lobby model. You need to trace this on the Windows box.
namespace CustomizationPath {
// Category offsets within TemporaryCharacter for ApplySkin's categoryOffset param.
inline constexpr std::uint16_t WeaponCategory   = 0x38; // from reference code
inline constexpr std::uint16_t BodyCategory     = 0x0;  // TODO
inline constexpr std::uint16_t MaskCategory     = 0x0;  // TODO
inline constexpr std::uint16_t BagCategory      = 0x0;  // TODO
inline constexpr std::uint16_t KeychainCategory = 0x0;  // TODO

inline const std::uint16_t CosmeticCategoryOffsets[kCosmeticCount] = {
    BodyCategory, BodyCategory, MaskCategory, BagCategory, KeychainCategory
};
} // namespace CustomizationPath

// ---- Helpers -------------------------------------------------------------

static std::uintptr_t ReadPtr(std::uintptr_t addr)
{
    return TargetProcess.Read<std::uintptr_t>(addr);
}

// Read a skin description pointer from a singleton's field.
// singleton is the description container (e.g. AllWeaponSkinDescriptions instance),
// fieldOffset is the field within it (e.g. 0x88 for Dp9S10).
static std::uintptr_t ReadSkinDescription(
    std::uintptr_t typeInfoRva,
    std::uintptr_t fieldOffset)
{
    if (!g_gaBase || !typeInfoRva)
        return 0;

    const std::uintptr_t klass = ReadPtr(g_gaBase + typeInfoRva);
    if (!klass) return 0;

    const std::uintptr_t staticFields = ReadPtr(klass + 0xB8);
    if (!staticFields) return 0;

    const std::uintptr_t singleton = ReadPtr(staticFields);
    if (!singleton) return 0;

    return ReadPtr(singleton + fieldOffset);
}

// Write the skin description into the customization pipeline to preview it.
// See reference code: writes to CustomizationLoader and bumps VersionModel.
static bool ApplySkin(
    std::uintptr_t customizationLoader,
    std::uintptr_t temporaryCharacter,
    std::uint16_t  categoryOffset,
    std::uintptr_t selectedDescription)
{
    if (!customizationLoader || !temporaryCharacter ||
        !categoryOffset || !selectedDescription)
        return false;

    const std::uintptr_t versionModel =
        ReadPtr(temporaryCharacter + 0x20);
    const std::uintptr_t temporaryValue =
        ReadPtr(temporaryCharacter + categoryOffset);

    if (!versionModel || !temporaryValue)
        return false;

    if (!TargetProcess.Write(customizationLoader + 0x18, temporaryCharacter))
        return false;
    if (!TargetProcess.Write(customizationLoader + 0x20, versionModel))
        return false;
    if (!TargetProcess.Write(temporaryValue + 0x18, selectedDescription))
        return false;

    std::int32_t version = TargetProcess.Read<std::int32_t>(versionModel + 0x10);
    version = (version == INT_MAX) ? 0 : version + 1;
    TargetProcess.Write(versionModel + 0x10, version);

    return true;
}

// ---- Public API ----------------------------------------------------------

void Init(std::uintptr_t gameAssemblyBase)
{
    g_gaBase = gameAssemblyBase;
}

void Shutdown()
{
    g_gaBase = 0;
}

void ApplySelectedSkins()
{
    if (!g_gaBase)
        return;
    if (!ConfigInstance.Skin.Enabled)
        return;

    // TODO: Resolve CustomizationLoader and TemporaryCharacter pointers.
    // The reference code uses these two objects, which are reachable from the
    // local player's lobby/wardrobe model at runtime. Trace the chain on the
    // Windows box and fill in the reads below.
    //
    // Something like:
    //   uintptr_t playerModel = ReadPtr(gaBase + rr::TypeInfo::???);
    //   uintptr_t customizationLoader = ReadPtr(playerModel + ???);
    //   uintptr_t temporaryCharacter  = ReadPtr(customizationLoader + ???);
    //
    std::uintptr_t customizationLoader = 0; // TODO
    std::uintptr_t temporaryCharacter  = 0; // TODO

    if (!customizationLoader || !temporaryCharacter)
        return;

    // ---- Weapon skins ----------------------------------------------------
    for (int w = 0; w < kWeaponCount; ++w) {
        const int sel = ConfigInstance.Skin.WeaponSkin[w];
        if (sel <= 0) continue; // 0 = no override

        const auto& cat = kWeaponCatalogs[w];
        const int idx = sel - 1; // config is 1-based (0 = off)
        if (idx >= cat.count) continue;

        const std::uintptr_t desc = ReadSkinDescription(
            DescriptionRVA::WeaponSkins,
            cat.skins[idx].offset);
        if (!desc) continue;

        ApplySkin(customizationLoader, temporaryCharacter,
                  CustomizationPath::WeaponCategory, desc);
    }

    // ---- Cosmetics -------------------------------------------------------
    const int cosmeticSelections[kCosmeticCount] = {
        ConfigInstance.Skin.OutfitMale,
        ConfigInstance.Skin.OutfitFemale,
        ConfigInstance.Skin.MaskSkin,
        ConfigInstance.Skin.BagSkin,
        ConfigInstance.Skin.KeychainSkin,
    };

    for (int c = 0; c < kCosmeticCount; ++c) {
        const int sel = cosmeticSelections[c];
        if (sel <= 0) continue;

        const auto& cat = kCosmeticCatalogs[c];
        const int idx = sel - 1;
        if (idx >= cat.count) continue;

        const std::uintptr_t rva = DescriptionRVA::CosmeticRVAs[c];
        if (!rva) continue; // not filled in yet

        const std::uintptr_t desc = ReadSkinDescription(rva, cat.skins[idx].offset);
        if (!desc) continue;

        const std::uint16_t catOff = CustomizationPath::CosmeticCategoryOffsets[c];
        if (!catOff) continue; // not filled in yet

        ApplySkin(customizationLoader, temporaryCharacter, catOff, desc);
    }
}

} // namespace SkinChanger
