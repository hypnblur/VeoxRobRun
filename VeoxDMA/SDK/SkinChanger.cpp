#include "pch.h"
#include "SkinChanger.h"
#include "RobRunSDK.h"
#include "RobRunMemory.h"
#include "../Config/ConfigInstance.h"
#include "../Memory/Memory.h"

extern Memory TargetProcess;

namespace SkinChanger {

static std::uintptr_t g_gaBase = 0;

// TypeInfo RVAs — look these up in script.json from Il2CppDumper on the
// Windows box. The TypeDefIndex (from dump.cs) is listed for each class.
// In the TypeInfoDefinitionTable, entry [TypeDefIndex] holds the pointer
// to the Il2CppClass. The RVA is the offset of that table entry in
// GameAssembly.dll.
namespace DescriptionRVA {

// WeaponSkinDescriptions — TypeDefIndex: 729
inline constexpr std::uintptr_t WeaponSkins = 0x0; // TODO

// CharacterBodyDescriptions — TypeDefIndex: 1071
inline constexpr std::uintptr_t CharacterBody = 0x0; // TODO

// CharacterFBodyDescriptions — TypeDefIndex: 1073
inline constexpr std::uintptr_t CharacterFBody = 0x0; // TODO

// CharacterMaskDescriptions — TypeDefIndex: 1085
inline constexpr std::uintptr_t CharacterMask = 0x0; // TODO

// CharacterBagDescriptions — TypeDefIndex: 1068
inline constexpr std::uintptr_t CharacterBag = 0x0; // TODO

// CharacterKeychainDescriptions — TypeDefIndex: 1083
inline constexpr std::uintptr_t CharacterKeychain = 0x0; // TODO

inline const std::uintptr_t CosmeticRVAs[kCosmeticCount] = {
    CharacterBody, CharacterFBody, CharacterMask, CharacterBag, CharacterKeychain
};
} // namespace DescriptionRVA

// CustomizationData field layout (from CustomizationModel._customizationData at +0x10):
//   Body     IPrimitiveData<CharacterBodyDescription>     +0x30
//   Bag      IPrimitiveData<CharacterBagDescription>      +0x78
//   Mask     IPrimitiveData<CharacterMaskDescription>     +0x80
//   Keychain IPrimitiveData<CharacterKeychainDescription> +0x98
//   FBody    IPrimitiveData<CharacterFBodyDescription>    +0xB0
//   FBag     IPrimitiveData<CharacterBagDescription>      +0xE8
//   FMask    IPrimitiveData<CharacterMaskDescription>     +0xF0
//   FKeychain IPrimitiveData<CharacterKeychainDescription>+0x120
// Each IPrimitiveData stores its value (description pointer) at +0x18.
namespace CustomizationPath {
inline constexpr std::uintptr_t CustomizationData = 0x10;
inline constexpr std::uintptr_t PrimitiveValue    = 0x18;

inline constexpr std::uintptr_t Body     = 0x30;
inline constexpr std::uintptr_t Bag      = 0x78;
inline constexpr std::uintptr_t Mask     = 0x80;
inline constexpr std::uintptr_t Keychain = 0x98;
inline constexpr std::uintptr_t FBody    = 0xB0;
inline constexpr std::uintptr_t FBag     = 0xE8;
inline constexpr std::uintptr_t FMask    = 0xF0;
inline constexpr std::uintptr_t FKeychain = 0x120;

inline const std::uintptr_t CosmeticDataOffsets[kCosmeticCount] = {
    Body, FBody, Mask, Bag, Keychain
};
} // namespace CustomizationPath

// ---- Helpers -------------------------------------------------------------

static std::uintptr_t ReadPtr(std::uintptr_t addr)
{
    return TargetProcess.Read<std::uintptr_t>(addr);
}

// Read a skin description pointer from a static Descriptions class.
// These are static classes — their fields sit directly in the staticFields block.
// klass->static_fields (at klass+0xB8) -> field at offset.
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

    return ReadPtr(staticFields + fieldOffset);
}

// Write a description pointer into a CustomizationData's IPrimitiveData slot.
// customizationData: pointer to the CustomizationData object
// slotOffset: offset of the IPrimitiveData field within CustomizationData (e.g. 0x30 for Body)
// descPtr: the description object pointer to write
static bool WriteCustomizationSlot(
    std::uintptr_t customizationData,
    std::uintptr_t slotOffset,
    std::uintptr_t descPtr)
{
    if (!customizationData || !descPtr)
        return false;

    const std::uintptr_t primitiveData = ReadPtr(customizationData + slotOffset);
    if (primitiveData < 0x10000)
        return false;

    return TargetProcess.Write(
        primitiveData + CustomizationPath::PrimitiveValue,
        descPtr);
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

    // TODO: Resolve the local player's CustomizationModel.
    // The chain from the dump:
    //   BattleCharacterModel (TypeDefIndex: ???) has ICustomizationModel at some offset
    //   CustomizationModel (TypeDefIndex: 4009) wraps CustomizationData at +0x10
    //   CustomizationData (TypeDefIndex: 4945) has the IPrimitiveData fields
    //
    // For weapon skins:
    //   WeaponSkinsModel (TypeDefIndex: 3344) has WeaponSkinsData at +0x10
    //   WeaponSkinsModel.SelectSkin(desc) is what the game calls
    //
    // You need to find how to reach these from the local player entity.
    // Something like:
    //   playerEntity -> publicModel -> CustomizationModel -> _customizationData
    //   playerEntity -> WeaponSkinsModel
    //
    std::uintptr_t customizationData = 0; // TODO: resolve from player
    std::uintptr_t weaponSkinsModel  = 0; // TODO: resolve from player

    // ---- Cosmetics -------------------------------------------------------
    if (customizationData) {
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
            if (!rva) continue;

            const std::uintptr_t desc = ReadSkinDescription(rva, cat.skins[idx].offset);
            if (!desc) continue;

            const std::uintptr_t slotOff = CustomizationPath::CosmeticDataOffsets[c];
            WriteCustomizationSlot(customizationData, slotOff, desc);
        }
    }

    // ---- Weapon skins ----------------------------------------------------
    // TODO: weapon skins are applied through WeaponSkinsModel.SelectSkin()
    // which writes to WeaponSkinsData internally. You need to reverse
    // WeaponSkinsData on the Windows box to find the selected-skin field
    // per weapon base, then DMA-write the description pointer there.
    if (weaponSkinsModel) {
        for (int w = 0; w < kWeaponCount; ++w) {
            const int sel = ConfigInstance.Skin.WeaponSkin[w];
            if (sel <= 0) continue;

            const auto& cat = kWeaponCatalogs[w];
            const int idx = sel - 1;
            if (idx >= cat.count) continue;

            const std::uintptr_t desc = ReadSkinDescription(
                DescriptionRVA::WeaponSkins,
                cat.skins[idx].offset);
            if (!desc) continue;

            // TODO: write desc to the correct slot in WeaponSkinsData
            (void)desc;
        }
    }
}

} // namespace SkinChanger
