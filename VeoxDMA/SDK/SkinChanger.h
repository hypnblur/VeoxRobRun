#pragma once
#include "pch.h"
#include <cstdint>
#include <vector>
#include <string>

namespace SkinChanger {

struct SkinEntry {
    const char* field;
    std::uintptr_t offset;
    const char* name;
};

// ---- Weapon categories ---------------------------------------------------

enum Weapon {
    Jp7, Dp9, Kl545, Lmg, Cb45, Harris500, Rc556, Starh,
    SrReaper, Pdw90, ApScalar, Ordr12, Lbr44, Tag12K,
    Drm14, Mc10, Vulture,
    kWeaponCount
};

inline const char* WeaponNames[] = {
    "JP-7", "DP-9", "KL-545", "M249 LMG", "CB45", "Harris 500",
    "RC 5.56", "Star-H", "SR Reaper", "PDW90", "AP Scalar",
    "ORDR-12", "LBR44", "TAG-12k", "DRM-14", "MC-10", "Vulture 50"
};

inline const SkinEntry kJp7Skins[] = {
    { "Jp7S0",  0x8,  "Factory Paint" },
    { "Jp7S1",  0x10, "Run Paint" },
    { "Jp7S2",  0x18, "Desert Paint" },
    { "Jp7S3",  0x20, "Gasoline Paint" },
    { "Jp7S4",  0x28, "Cutie Paint" },
    { "Jp7S5",  0x30, "Focus Paint" },
};

inline const SkinEntry kDp9Skins[] = {
    { "Dp9S0",  0x38, "Factory Paint" },
    { "Dp9S1",  0x40, "Paint 1" },
    { "Dp9S2",  0x48, "Flame Paint" },
    { "Dp9S3",  0x50, "Paint 3" },
    { "Dp9S4",  0x58, "Paint 4" },
    { "Dp9S5",  0x60, "Prototype Paint" },
    { "Dp9S6",  0x68, "Reckless Paint" },
    { "Dp9S7",  0x70, "Strike Paint" },
    { "Dp9S8",  0x78, "Makarov Paint" },
    { "Dp9S9",  0x80, "DIY Paint" },
    { "Dp9S10", 0x88, "Ichigeki Paint" },
    { "Dp9S12", 0x90, "Nya Paint" },
};

inline const SkinEntry kKl545Skins[] = {
    { "Kl545S0",  0x98,  "Factory Paint" },
    { "Kl545S2",  0xA8,  "Experience Paint" },
    { "Kl545S3",  0xB0,  "Tropics Paint" },
    { "Kl545S4",  0xB8,  "Overseer Paint" },
    { "Kl545S5",  0xC0,  "Paint 5" },
    { "Kl545S6",  0xC8,  "Paint 6" },
    { "Kl545S7",  0xD0,  "Paint 7" },
    { "Kl545S8",  0xD8,  "Midas Paint" },
    { "Kl545S9",  0xE0,  "Paint 9" },
    { "Kl545S10", 0xE8,  "Paint 10" },
    { "Kl545S11", 0xF0,  "Paint 11" },
    { "Kl545S12", 0xF8,  "Chameleon Paint" },
    { "Kl545S13", 0x100, "Cuts Paint" },
    { "Kl545S14", 0x108, "Business Paint" },
    { "Kl545S15", 0x110, "Limit Paint" },
};

inline const SkinEntry kLmgSkins[] = {
    { "LmgS0", 0xA0, "Factory Paint" },
};

inline const SkinEntry kCb45Skins[] = {
    { "Cb45S0", 0x118, "Factory Paint" },
    { "Cb45S1", 0x120, "Symphony Paint" },
    { "Cb45S2", 0x128, "Paint 2" },
    { "Cb45S3", 0x130, "Ambition Paint" },
    { "Cb45S4", 0x138, "Paint 4" },
};

inline const SkinEntry kHarris500Skins[] = {
    { "Harris500S0", 0x140, "Factory Paint" },
    { "Harris500S1", 0x148, "Judge Paint" },
    { "Harris500S2", 0x150, "Thunderstrike Paint" },
    { "Harris500S3", 0x158, "Paint 3" },
    { "Harris500S4", 0x160, "Pharaoh Paint" },
    { "Harris500S5", 0x168, "Polaris Paint" },
    { "Harris500S6", 0x170, "Paws Paint" },
};

inline const SkinEntry kRc556Skins[] = {
    { "Rc556S0", 0x178, "Factory Paint" },
    { "Rc556S1", 0x180, "Stars and Stripes Paint" },
    { "Rc556S2", 0x188, "Shadow Paint" },
    { "Rc556S3", 0x190, "Paint 3" },
    { "Rc556S4", 0x198, "Paint 4" },
    { "Rc556S5", 0x1A0, "Arrow Paint" },
    { "Rc556S7", 0x1A8, "Paint 7" },
    { "Rc556S6", 0x1B0, "Paint 6" },
    { "Rc556S8", 0x1B8, "Acid Paint" },
};

inline const SkinEntry kStarhSkins[] = {
    { "StarhS0", 0x1C0, "Factory Paint" },
};

inline const SkinEntry kSrReaperSkins[] = {
    { "SrReaperS0", 0x1C8, "Factory Paint" },
    { "SrReaperS1", 0x1D0, "Paint 1" },
    { "SrReaperS2", 0x1D8, "Paint 2" },
    { "SrReaperS3", 0x1E0, "Paint 3" },
    { "SrReaperS4", 0x1E8, "Endurance Paint" },
    { "SrReaperS5", 0x1F0, "Paint 5" },
    { "SrReaperS6", 0x1F8, "Paint 6" },
    { "SrReaperS7", 0x200, "Threshold Paint" },
};

inline const SkinEntry kPdw90Skins[] = {
    { "Pdw90S0", 0x208, "Factory Paint" },
    { "Pdw90S1", 0x210, "Rhyme Paint" },
    { "Pdw90S2", 0x218, "Stealth Paint" },
    { "Pdw90S3", 0x220, "Paint 3" },
    { "Pdw90S4", 0x228, "Paint 4" },
    { "Pdw90S5", 0x230, "Mineral Paint" },
    { "Pdw90S6", 0x238, "Paint 6" },
    { "Pdw90S7", 0x240, "Mirage Paint" },
};

inline const SkinEntry kApScalarSkins[] = {
    { "ApScalarS0", 0x248, "Factory Paint" },
    { "ApScalarS1", 0x250, "Predator Paint" },
    { "ApScalarS2", 0x258, "Era Paint" },
    { "ApScalarS3", 0x260, "Paint 3" },
};

inline const SkinEntry kOrdr12Skins[] = {
    { "Ordr12S0", 0x268, "Factory Paint" },
};

inline const SkinEntry kLbr44Skins[] = {
    { "Lbr44S0", 0x270, "Factory Paint" },
    { "Lbr44S1", 0x278, "Atonement Paint" },
    { "Lbr44S2", 0x280, "Paint 2" },
    { "Lbr44S3", 0x288, "Western Paint" },
};

inline const SkinEntry kTag12KSkins[] = {
    { "Tag12Ks0", 0x290, "Factory Paint" },
    { "Tag12Ks1", 0x298, "Toxic Paint" },
    { "Tag12Ks2", 0x2A0, "Pathogen Paint" },
    { "Tag12Ks3", 0x2A8, "Paint 3" },
    { "Tag12Ks4", 0x2B0, "Paint 4" },
    { "Tag12Ks5", 0x2B8, "Quick Sand Paint" },
    { "Tag12Ks6", 0x2C0, "Agent Paint" },
    { "Tag12Ks7", 0x2C8, "Paint 7" },
    { "Tag12Ks8", 0x2D0, "Paint 8" },
};

inline const SkinEntry kDrm14Skins[] = {
    { "Drm14S0", 0x2D8, "Factory Paint" },
    { "Drm14S1", 0x2E0, "Paint 1" },
    { "Drm14S2", 0x2E8, "Cobra Paint" },
    { "Drm14S3", 0x2F0, "Paint 3" },
    { "Drm14S4", 0x2F8, "Wolf Den Paint" },
    { "Drm14S5", 0x300, "Paint 5" },
    { "Drm14S6", 0x308, "Garage Paint" },
};

inline const SkinEntry kMc10Skins[] = {
    { "Mc10S0", 0x310, "Factory Paint" },
    { "Mc10S1", 0x318, "Paint 1" },
    { "Mc10S2", 0x320, "Paint 2" },
    { "Mc10S3", 0x328, "Flash Paint" },
    { "Mc10S4", 0x330, "Paint 4" },
    { "Mc10S5", 0x338, "Paint 5" },
    { "Mc10S6", 0x340, "Prism Paint" },
    { "Mc10S7", 0x348, "Pungent Paint" },
    { "Mc10S8", 0x350, "Rensha Paint" },
    { "Mc10S9", 0x358, "Paint 9" },
};

inline const SkinEntry kVultureSkins[] = {
    { "VultureS0", 0x360, "Factory Paint" },
    { "VultureS3", 0x368, "Laguna Paint" },
};

struct WeaponCatalog {
    const SkinEntry* skins;
    int count;
};

inline const WeaponCatalog kWeaponCatalogs[kWeaponCount] = {
    { kJp7Skins,       (int)std::size(kJp7Skins) },
    { kDp9Skins,       (int)std::size(kDp9Skins) },
    { kKl545Skins,     (int)std::size(kKl545Skins) },
    { kLmgSkins,       (int)std::size(kLmgSkins) },
    { kCb45Skins,      (int)std::size(kCb45Skins) },
    { kHarris500Skins, (int)std::size(kHarris500Skins) },
    { kRc556Skins,     (int)std::size(kRc556Skins) },
    { kStarhSkins,     (int)std::size(kStarhSkins) },
    { kSrReaperSkins,  (int)std::size(kSrReaperSkins) },
    { kPdw90Skins,     (int)std::size(kPdw90Skins) },
    { kApScalarSkins,  (int)std::size(kApScalarSkins) },
    { kOrdr12Skins,    (int)std::size(kOrdr12Skins) },
    { kLbr44Skins,     (int)std::size(kLbr44Skins) },
    { kTag12KSkins,    (int)std::size(kTag12KSkins) },
    { kDrm14Skins,     (int)std::size(kDrm14Skins) },
    { kMc10Skins,      (int)std::size(kMc10Skins) },
    { kVultureSkins,   (int)std::size(kVultureSkins) },
};

// ---- Cosmetic categories -------------------------------------------------

enum Cosmetic {
    OutfitMale, OutfitFemale, Mask, Bag, Keychain,
    kCosmeticCount
};

inline const char* CosmeticNames[] = {
    "Outfit (Male)", "Outfit (Female)", "Mask", "Bag", "Keychain"
};

inline const SkinEntry kOutfitMaleSkins[] = {
    { "Body2Type1Paint1",  0x8,   "Classic Sporty" },
    { "Body2Type1Paint2",  0x10,  "Bucks Sporty" },
    { "Body2Type1Paint3",  0x18,  "Rust Sporty" },
    { "Body2Type1Paint4",  0x20,  "Smoke Sporty" },
    { "Body3Type1Paint2",  0x30,  "AKINA Sporty" },
    { "Body3Type1Paint3",  0x38,  "Wolf Sporty" },
    { "Body3Type1Paint4",  0x40,  "Energy Sporty" },
    { "Body5Type1Paint1",  0x48,  "New Chapter" },
    { "Body12Type1Paint1", 0x118, "Apex" },
    { "Body16Type1Paint1", 0x168, "Hornet Cook" },
    { "Body16Type1Paint2", 0x170, "Sky Cook" },
    { "Body16Type1Paint3", 0x178, "Sea Cook" },
    { "Body18Type1Paint1", 0x190, "Noise Street" },
    { "Body18Type1Paint2", 0x198, "Tone Street" },
    { "Body18Type1Paint3", 0x1A0, "Heat Street" },
    { "Body18Type1Paint4", 0x1A8, "Angle Street" },
    { "Body19Type1Paint1", 0x1B0, "Shadow Freedom" },
    { "Body19Type1Paint2", 0x1B8, "Hail Freedom" },
    { "Body22Type2Paint2", 0x1F0, "Outfit 22 - V3" },
    { "Body24Type1Paint1", 0x210, "Farmer Country" },
    { "Body24Type1Paint2", 0x218, "Hunter Country" },
    { "Body24Type1Paint3", 0x220, "Lumberjack Country" },
    { "Body28Type1Paint1", 0x248, "Sinner" },
    { "Body30Type1Paint1", 0x258, "Snow Cartel" },
    { "Body30Type1Paint2", 0x260, "Sea Cartel" },
    { "Body31Type1Paint1", 0x268, "Drive Retreat" },
    { "Body31Type2Paint1", 0x270, "Style Retreat" },
    { "Body33Type2Paint1", 0x288, "Power" },
    { "Body42Type1Paint1", 0x2B8, "Sports Jacket" },
    { "Body42Type1Paint2", 0x2C8, "Black Sports Jacket" },
    { "Body42Type1Paint3", 0x2C0, "Blue Sports Jacket" },
    { "Body46Type1Paint1", 0x2D0, "Outfit 46" },
};

inline const SkinEntry kOutfitFemaleSkins[] = {
    { "Body2Type1Paint1",  0x8,   "Classic Sporty" },
    { "Body2Type1Paint2",  0x10,  "Bucks Sporty" },
    { "Body2Type1Paint3",  0x18,  "Rust Sporty" },
    { "Body2Type1Paint4",  0x20,  "Smoke Sporty" },
    { "Body3Type2Paint1",  0x30,  "Outfit 3 - V2" },
    { "Body3Type2Paint2",  0x38,  "Outfit 3 - V3" },
    { "Body3Type2Paint3",  0x40,  "Outfit 3 - V4" },
    { "Body5Type1Paint1",  0x50,  "New Chapter" },
    { "Body16Type1Paint1", 0x118, "Hornet Cook" },
    { "Body16Type1Paint2", 0x120, "Sky Cook" },
    { "Body16Type1Paint3", 0x128, "Sea Cook" },
    { "Body18Type1Paint1", 0x138, "1997 Street" },
    { "Body18Type1Paint2", 0x140, "Khaki Street" },
    { "Body18Type1Paint3", 0x148, "Wasp Street" },
    { "Body18Type1Paint4", 0x150, "INSTINCT Street" },
    { "Body19Type1Paint1", 0x158, "Shadow Freedom" },
    { "Body19Type2Paint2", 0x168, "Storm Freedom" },
    { "Body22Type2Paint1", 0x198, "SPARTA Athlete" },
    { "Body24Type1Paint1", 0x1B0, "Farmer Country" },
    { "Body24Type1Paint2", 0x1B8, "Hunter Country" },
    { "Body24Type1Paint3", 0x1C0, "Lumberjack Country" },
    { "Body28Type1Paint1", 0x1E8, "Sinner" },
    { "Body30Type1Paint1", 0x1F8, "Snow Cartel" },
    { "Body30Type1Paint2", 0x200, "Sea Cartel" },
    { "Body31Type1Paint1", 0x208, "Drive Retreat" },
    { "Body31Type2Paint1", 0x210, "Style Retreat" },
    { "Body33Type2Paint1", 0x228, "Power" },
    { "Body35Type1Paint1", 0x250, "Raid" },
    { "Body42Type1Paint1", 0x270, "Sports Jacket" },
    { "Body42Type1Paint2", 0x278, "Black Sports Jacket" },
    { "Body42Type1Paint3", 0x280, "Blue Sports Jacket" },
    { "Body47Type1Paint1", 0x288, "Apex" },
};

inline const SkinEntry kMaskSkins[] = {
    { "Mask3Type1Paint1",  0x48,  "Black Ballistic" },
    { "Mask3Type1Paint2",  0x50,  "Carbon Ballistic" },
    { "Mask6Type1Paint2",  0x88,  "Flames Welder" },
    { "Mask7Type1Paint1",  0xA8,  "Respirator" },
    { "Mask9Type1Paint3",  0xC0,  "Biker Teeth" },
    { "Mask10Type1Paint1", 0xD0,  "Pink Gas Mask" },
    { "Mask10Type1Paint2", 0xD8,  "Blue Gas Mask" },
    { "Mask11Type1Paint1", 0xE0,  "Demon" },
    { "Mask12Type1Paint1", 0xE8,  "Mask 12" },
    { "Mask13Type1Paint1", 0xF0,  "Gold Doll" },
    { "Mask15Type1Paint1", 0x100, "Gold Skull" },
    { "Mask16Type1Paint2", 0x110, "Green Lover" },
    { "Mask18Type1Paint1", 0x120, "Skull Respirator" },
    { "Mask19Type1Paint1", 0x128, "Mime Doll" },
    { "Mask20Type1Paint2", 0x138, "Pink Biker" },
    { "Mask21Type1Paint1", 0x140, "Star Horn" },
    { "Mask27Type1Paint1", 0x198, "Black Bucket Hat" },
    { "Mask27Type1Paint2", 0x1A0, "Yellow Bucket Hat" },
    { "Mask28Type1Paint1", 0x1B8, "Urban Biker" },
    { "Mask31Type1Paint1", 0x1C8, "Wild West" },
    { "Mask33Type1Paint1", 0x1D8, "Panther" },
    { "Mask34Type1Paint1", 0x1E0, "Cap" },
    { "Mask36Type1Paint1", 0x1E8, "Fan" },
    { "Mask38Type1Paint1", 0x1F8, "Fearsome" },
    { "Mask38Type1Paint2", 0x200, "Madman" },
    { "Mask41Type1Paint1", 0x218, "Silence" },
    { "Mask44Type1Paint1", 0x240, "Concrete Gang" },
    { "Mask44Type1Paint2", 0x248, "Gray Gang" },
    { "Mask44Type1Paint3", 0x250, "Graffiti Gang" },
};

inline const SkinEntry kBagSkins[] = {
    { "Bag1Type1Paint1",  0x8,   "Classic Sporty" },
    { "Bag7Type1Paint1",  0x48,  "Orange Sporty" },
    { "Bag7Type1Paint2",  0x50,  "Tactics Sporty" },
    { "Bag7Type1Paint3",  0x58,  "Edges Sporty" },
    { "Bag9Type1Paint1",  0x68,  "Crocodile Premium" },
    { "Bag12Type1Paint1", 0x88,  "Lady Urban" },
    { "Bag13Type1Paint1", 0x90,  "Red Demon" },
    { "Bag13Type2Paint1", 0x98,  "Brown Demon" },
    { "Bag14Type1Paint1", 0xA0,  "Black Urban" },
    { "Bag15Type1Paint1", 0xB0,  "DANGER Chemical" },
    { "Bag15Type1Paint2", 0xB8,  "MATRIX Chemical" },
    { "Bag18Type1Paint1", 0xE0,  "Independence" },
    { "Bag18Type1Paint2", 0xE8,  "Red Hill" },
    { "Bag21Type1Paint1", 0x100, "Duffel" },
    { "Bag23Type1Paint1", 0x118, "Leopard" },
    { "Bag26Type1Paint1", 0x130, "Klain" },
    { "Bag32Type1Paint1", 0x168, "Extreme" },
    { "Bag33Type1Paint1", 0x170, "Fluffy Street" },
};

inline const SkinEntry kKeychainSkins[] = {
    { "Keychain4Paint1",  0x28,  "RIP" },
    { "Keychain15Paint1", 0x78,  "Like" },
    { "Keychain16Paint1", 0x80,  "Peace" },
    { "Keychain17Paint1", 0x88,  "Heart" },
    { "Keychain19Paint1", 0x98,  "Alien" },
    { "Keychain25Paint1", 0xD0,  "Candy" },
    { "Keychain29Paint1", 0xF0,  "Brass Knuckles" },
    { "Keychain39Paint1", 0x138, "Flask" },
    { "Keychain42Paint1", 0x150, "Bullet" },
    { "Keychain45Paint1", 0x168, "Exhibit" },
    { "Keychain46Paint1", 0x170, "Meat Grinder" },
    { "Keychain47Paint1", 0x178, "Canister" },
    { "Keychain48Paint1", 0x180, "Cellphone" },
    { "Keychain49Paint1", 0x188, "Whistle" },
    { "Keychain52Paint1", 0x198, "Little Paw" },
};

struct CosmeticCatalog {
    const SkinEntry* skins;
    int count;
};

inline const CosmeticCatalog kCosmeticCatalogs[kCosmeticCount] = {
    { kOutfitMaleSkins,   (int)std::size(kOutfitMaleSkins) },
    { kOutfitFemaleSkins, (int)std::size(kOutfitFemaleSkins) },
    { kMaskSkins,         (int)std::size(kMaskSkins) },
    { kBagSkins,          (int)std::size(kBagSkins) },
    { kKeychainSkins,     (int)std::size(kKeychainSkins) },
};

// ---- Runtime API ---------------------------------------------------------

void Init(std::uintptr_t gameAssemblyBase);
void Shutdown();

// Called each frame from the reader loop when the skin changer is enabled.
// Reads the customization pipeline pointers and applies selected skins.
void ApplySelectedSkins();

} // namespace SkinChanger
