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
// All weapon skins live in WeaponSkinDescriptions (TypeDefIndex 729).
// Offsets are static-field offsets within that class's staticFields block.

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
    { "Jp7S0", 0x8,  "Factory Paint" },
    { "Jp7S1", 0x10, "Run Paint" },
    { "Jp7S2", 0x18, "Desert Paint" },
    { "Jp7S3", 0x20, "Gasoline Paint" },
    { "Jp7S4", 0x28, "Cutie Paint" },
    { "Jp7S5", 0x30, "Focus Paint" },
};

inline const SkinEntry kDp9Skins[] = {
    { "Dp9S0", 0x38, "Factory Paint" },
    { "Dp9S1", 0x40, "Paint 1" },
    { "Dp9S2", 0x48, "Flame Paint" },
    { "Dp9S3", 0x50, "Paint 3" },
    { "Dp9S4", 0x58, "Paint 4" },
    { "Dp9S5", 0x60, "Prototype Paint" },
    { "Dp9S6", 0x68, "Reckless Paint" },
    { "Dp9S7", 0x70, "Strike Paint" },
    { "Dp9S8", 0x78, "Makarov Paint" },
    { "Dp9S9", 0x80, "DIY Paint" },
};

inline const SkinEntry kKl545Skins[] = {
    { "Kl545S0",  0x88,  "Factory Paint" },
    { "Kl545S2",  0x98,  "Experience Paint" },
    { "Kl545S3",  0xA0,  "Tropics Paint" },
    { "Kl545S4",  0xA8,  "Overseer Paint" },
    { "Kl545S5",  0xB0,  "Paint 5" },
    { "Kl545S6",  0xB8,  "Paint 6" },
    { "Kl545S7",  0xC0,  "Paint 7" },
    { "Kl545S8",  0xC8,  "Midas Paint" },
    { "Kl545S9",  0xD0,  "Paint 9" },
    { "Kl545S10", 0xD8,  "Paint 10" },
    { "Kl545S11", 0xE0,  "Paint 11" },
    { "Kl545S12", 0xE8,  "Chameleon Paint" },
    { "Kl545S13", 0xF0,  "Cuts Paint" },
};

inline const SkinEntry kLmgSkins[] = {
    { "LmgS0", 0x90, "Factory Paint" },
};

inline const SkinEntry kCb45Skins[] = {
    { "Cb45S0", 0xF8,  "Factory Paint" },
    { "Cb45S1", 0x100, "Symphony Paint" },
    { "Cb45S2", 0x108, "Paint 2" },
    { "Cb45S3", 0x110, "Ambition Paint" },
    { "Cb45S4", 0x118, "Paint 4" },
};

inline const SkinEntry kHarris500Skins[] = {
    { "Harris500S0", 0x120, "Factory Paint" },
    { "Harris500S1", 0x128, "Judge Paint" },
    { "Harris500S2", 0x130, "Thunderstrike Paint" },
    { "Harris500S3", 0x138, "Paint 3" },
    { "Harris500S4", 0x140, "Pharaoh Paint" },
    { "Harris500S5", 0x148, "Polaris Paint" },
};

inline const SkinEntry kRc556Skins[] = {
    { "Rc556S0", 0x150, "Factory Paint" },
    { "Rc556S1", 0x158, "Stars and Stripes Paint" },
    { "Rc556S2", 0x160, "Shadow Paint" },
    { "Rc556S3", 0x168, "Paint 3" },
    { "Rc556S4", 0x170, "Paint 4" },
    { "Rc556S5", 0x178, "Arrow Paint" },
    { "Rc556S7", 0x180, "Paint 7" },
    { "Rc556S6", 0x188, "Paint 6" },
    { "Rc556S8", 0x190, "Acid Paint" },
};

inline const SkinEntry kStarhSkins[] = {
    { "StarhS0", 0x198, "Factory Paint" },
};

inline const SkinEntry kSrReaperSkins[] = {
    { "SrReaperS0", 0x1A0, "Factory Paint" },
    { "SrReaperS1", 0x1A8, "Paint 1" },
    { "SrReaperS2", 0x1B0, "Paint 2" },
    { "SrReaperS3", 0x1B8, "Paint 3" },
    { "SrReaperS4", 0x1C0, "Endurance Paint" },
    { "SrReaperS5", 0x1C8, "Paint 5" },
    { "SrReaperS6", 0x1D0, "Paint 6" },
};

inline const SkinEntry kPdw90Skins[] = {
    { "Pdw90S0", 0x1D8, "Factory Paint" },
    { "Pdw90S1", 0x1E0, "Rhyme Paint" },
    { "Pdw90S2", 0x1E8, "Stealth Paint" },
    { "Pdw90S3", 0x1F0, "Paint 3" },
    { "Pdw90S4", 0x1F8, "Paint 4" },
    { "Pdw90S5", 0x200, "Mineral Paint" },
    { "Pdw90S6", 0x208, "Paint 6" },
    { "Pdw90S7", 0x210, "Mirage Paint" },
};

inline const SkinEntry kApScalarSkins[] = {
    { "ApScalarS0", 0x218, "Factory Paint" },
    { "ApScalarS1", 0x220, "Predator Paint" },
    { "ApScalarS2", 0x228, "Era Paint" },
    { "ApScalarS3", 0x230, "Paint 3" },
};

inline const SkinEntry kOrdr12Skins[] = {
    { "Ordr12S0", 0x238, "Factory Paint" },
};

inline const SkinEntry kLbr44Skins[] = {
    { "Lbr44S0", 0x240, "Factory Paint" },
    { "Lbr44S1", 0x248, "Atonement Paint" },
    { "Lbr44S2", 0x250, "Paint 2" },
    { "Lbr44S3", 0x258, "Western Paint" },
};

inline const SkinEntry kTag12KSkins[] = {
    { "Tag12Ks0", 0x260, "Factory Paint" },
    { "Tag12Ks1", 0x268, "Toxic Paint" },
    { "Tag12Ks2", 0x270, "Pathogen Paint" },
    { "Tag12Ks3", 0x278, "Paint 3" },
    { "Tag12Ks4", 0x280, "Paint 4" },
    { "Tag12Ks5", 0x288, "Quick Sand Paint" },
    { "Tag12Ks6", 0x290, "Agent Paint" },
    { "Tag12Ks7", 0x298, "Paint 7" },
    { "Tag12Ks8", 0x2A0, "Paint 8" },
};

inline const SkinEntry kDrm14Skins[] = {
    { "Drm14S0", 0x2A8, "Factory Paint" },
    { "Drm14S1", 0x2B0, "Paint 1" },
    { "Drm14S2", 0x2B8, "Cobra Paint" },
    { "Drm14S3", 0x2C0, "Paint 3" },
    { "Drm14S4", 0x2C8, "Wolf Den Paint" },
    { "Drm14S5", 0x2D0, "Paint 5" },
    { "Drm14S6", 0x2D8, "Garage Paint" },
};

inline const SkinEntry kMc10Skins[] = {
    { "Mc10S0", 0x2E0, "Factory Paint" },
    { "Mc10S1", 0x2E8, "Paint 1" },
    { "Mc10S2", 0x2F0, "Paint 2" },
    { "Mc10S3", 0x2F8, "Flash Paint" },
    { "Mc10S4", 0x300, "Paint 4" },
    { "Mc10S5", 0x308, "Paint 5" },
    { "Mc10S6", 0x310, "Prism Paint" },
    { "Mc10S7", 0x318, "Pungent Paint" },
};

inline const SkinEntry kVultureSkins[] = {
    { "VultureS0", 0x320, "Factory Paint" },
    { "VultureS3", 0x328, "Laguna Paint" },
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
// Male bodies in CharacterBodyDescriptions   (TypeDefIndex 1071)
// Female bodies in CharacterFBodyDescriptions (TypeDefIndex 1073)
// Masks in CharacterMaskDescriptions          (TypeDefIndex 1085)
// Bags in CharacterBagDescriptions            (TypeDefIndex 1068)
// Keychains in CharacterKeychainDescriptions  (TypeDefIndex 1083)

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
    { "Body3Type1Paint1",  0x28,  "Sporty V1" },
    { "Body3Type1Paint2",  0x30,  "AKINA Sporty" },
    { "Body3Type1Paint3",  0x38,  "Wolf Sporty" },
    { "Body3Type1Paint4",  0x40,  "Energy Sporty" },
    { "Body5Type1Paint1",  0x48,  "New Chapter" },
    { "Body6Type1Paint1",  0x50,  "Classic Suit" },
    { "Body6Type1Paint2",  0x58,  "Alt Suit" },
    { "Body6Type1Paint3",  0x60,  "Dark Suit" },
    { "Body6Type1Paint4",  0x68,  "Suit V4" },
    { "Body7Type1Paint1",  0x80,  "Casual V1" },
    { "Body7Type1Paint2",  0x88,  "Casual V2" },
    { "Body7Type1Paint3",  0x90,  "Casual V3" },
    { "Body8Type1Paint1",  0xA0,  "Tactical V1" },
    { "Body8Type1Paint2",  0xA8,  "Tactical V2" },
    { "Body8Type1Paint3",  0xB0,  "Tactical V3" },
    { "Body8Type1Paint4",  0xB8,  "Tactical V4" },
    { "Body9Type1Paint1",  0xC0,  "Mil V1" },
    { "Body9Type1Paint2",  0xC8,  "Mil V2" },
    { "Body9Type1Paint3",  0xD0,  "Mil V3" },
    { "Body9Type1Paint4",  0xD8,  "Mil V4" },
    { "Body10Type1Paint1", 0xE0,  "Sport V1" },
    { "Body11Type1Paint1", 0xF8,  "Hoodie V1" },
    { "Body11Type1Paint2", 0x100, "Hoodie V2" },
    { "Body11Type1Paint3", 0x108, "Hoodie V3" },
    { "Body11Type1Paint4", 0x110, "Hoodie V4" },
    { "Body12Type1Paint1", 0x118, "Apex" },
    { "Body13Type1Paint1", 0x120, "Jacket V1" },
    { "Body13Type1Paint2", 0x128, "Jacket V2" },
    { "Body13Type1Paint3", 0x130, "Jacket V3" },
    { "Body13Type1Paint4", 0x138, "Jacket V4" },
    { "Body14Type1Paint1", 0x140, "Coat V1" },
    { "Body14Type1Paint2", 0x148, "Coat V2" },
    { "Body14Type1Paint3", 0x150, "Coat V3" },
    { "Body14Type1Paint4", 0x158, "Coat V4" },
    { "Body15Type1Paint1", 0x160, "Vest" },
    { "Body16Type1Paint1", 0x168, "Hornet Cook" },
    { "Body16Type1Paint2", 0x170, "Sky Cook" },
    { "Body16Type1Paint3", 0x178, "Sea Cook" },
    { "Body17Type1Paint1", 0x180, "Worker V1" },
    { "Body17Type2Paint1", 0x188, "Worker V2" },
    { "Body18Type1Paint1", 0x190, "Noise Street" },
    { "Body18Type1Paint2", 0x198, "Tone Street" },
    { "Body18Type1Paint3", 0x1A0, "Heat Street" },
    { "Body18Type1Paint4", 0x1A8, "Angle Street" },
    { "Body19Type1Paint1", 0x1B0, "Shadow Freedom" },
    { "Body19Type1Paint2", 0x1B8, "Hail Freedom" },
    { "Body20Type1Paint1", 0x1D0, "Outfit 20" },
    { "Body21Type1Paint1", 0x1D8, "Outfit 21" },
    { "Body22Type1Paint1", 0x1E0, "Athlete V1" },
    { "Body22Type2Paint1", 0x1E8, "Athlete V2" },
    { "Body22Type3Paint1", 0x1F0, "Athlete V3" },
    { "Body23Type1Paint1", 0x1F8, "Punk V1" },
    { "Body23Type1Paint2", 0x200, "Punk V2" },
    { "Body24Type1Paint1", 0x208, "Farmer Country" },
    { "Body24Type1Paint2", 0x210, "Hunter Country" },
    { "Body24Type1Paint3", 0x218, "Lumberjack Country" },
    { "Body25Type1Paint1", 0x220, "Outfit 25" },
    { "Body26Type1Paint1", 0x228, "Outfit 26" },
    { "Body27Type1Paint1", 0x230, "Outfit 27 V1" },
    { "Body27Type1Paint2", 0x238, "Outfit 27 V2" },
    { "Body28Type1Paint1", 0x240, "Sinner" },
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
};

inline const SkinEntry kMaskSkins[] = {
    { "Mask1Type1Paint1",  0x8,   "Balaclava V1" },
    { "Mask1Type1Paint2",  0x10,  "Balaclava V2" },
    { "Mask1Type1Paint3",  0x18,  "Balaclava V3" },
    { "Mask1Type1Paint4",  0x20,  "Balaclava V4" },
    { "Mask2Type1Paint1",  0x28,  "Ski Mask V1" },
    { "Mask2Type1Paint2",  0x30,  "Ski Mask V2" },
    { "Mask2Type1Paint3",  0x38,  "Ski Mask V3" },
    { "Mask2Type1Paint4",  0x40,  "Ski Mask V4" },
    { "Mask3Type1Paint1",  0x48,  "Black Ballistic" },
    { "Mask3Type1Paint2",  0x50,  "Carbon Ballistic" },
    { "Mask3Type1Paint3",  0x58,  "Ballistic V3" },
    { "Mask3Type1Paint4",  0x60,  "Ballistic V4" },
    { "Mask4Type1Paint1",  0x68,  "Mask 4" },
    { "Mask5Type1Paint1",  0x70,  "Half Mask V1" },
    { "Mask5Type2Paint1",  0x78,  "Half Mask V2" },
    { "Mask6Type1Paint1",  0x80,  "Welder V1" },
    { "Mask6Type1Paint2",  0x88,  "Flames Welder" },
    { "Mask6Type1Paint3",  0x90,  "Welder V3" },
    { "Mask6Type1Paint4",  0x98,  "Welder V4" },
    { "Mask6Type1Paint5",  0xA0,  "Welder V5" },
    { "Mask7Type1Paint1",  0xA8,  "Respirator" },
    { "Mask9Type1Paint1",  0xB0,  "Biker V1" },
    { "Mask9Type1Paint2",  0xB8,  "Biker V2" },
    { "Mask9Type1Paint3",  0xC0,  "Biker Teeth" },
    { "Mask9Type1Paint4",  0xC8,  "Biker V4" },
    { "Mask10Type1Paint1", 0xD0,  "Pink Gas Mask" },
    { "Mask10Type1Paint2", 0xD8,  "Blue Gas Mask" },
    { "Mask11Type1Paint1", 0xE0,  "Demon" },
    { "Mask12Type1Paint1", 0xE8,  "Mask 12" },
    { "Mask13Type1Paint1", 0xF0,  "Gold Doll" },
    { "Mask14Type1Paint1", 0xF8,  "Mask 14" },
    { "Mask15Type1Paint1", 0x100, "Gold Skull" },
    { "Mask16Type1Paint1", 0x108, "Lover V1" },
    { "Mask16Type1Paint2", 0x110, "Green Lover" },
    { "Mask17Type1aint1",  0x118, "Mask 17" },
    { "Mask18Type1Paint1", 0x120, "Skull Respirator" },
    { "Mask19Type1Paint1", 0x128, "Mime Doll" },
    { "Mask20Type1Paint1", 0x130, "Biker V1" },
    { "Mask21Type1Paint1", 0x138, "Star Horn" },
    { "Mask22Type1Paint1", 0x140, "Mask 22" },
    { "Mask23Type1Paint1", 0x148, "Mask 23" },
    { "Mask24Type1Paint1", 0x150, "Cap V1" },
    { "Mask24Type1Paint2", 0x158, "Cap V2" },
    { "Mask25Type1Paint1", 0x160, "Mask 25 V1" },
    { "Mask25Type1Paint2", 0x168, "Mask 25 V2" },
    { "Mask26Type1Paint1", 0x170, "Bucket V1" },
    { "Mask26Type1Paint2", 0x178, "Bucket V2" },
    { "Mask26Type1Paint3", 0x180, "Bucket V3" },
    { "Mask26Type2Paint1", 0x188, "Bucket Alt" },
    { "Mask27Type1Paint1", 0x190, "Black Bucket Hat" },
    { "Mask27Type1Paint2", 0x198, "Yellow Bucket Hat" },
    { "Mask27Type1Paint3", 0x1A0, "Bucket Hat V3" },
    { "Mask27Type1Paint4", 0x1A8, "Bucket Hat V4" },
    { "Mask28Type1Paint1", 0x1B0, "Urban Biker" },
    { "Mask29Type1Paint1", 0x1B8, "Mask 29" },
    { "Mask31Type1Paint1", 0x1C0, "Wild West" },
    { "Mask32Type1Paint1", 0x1C8, "Mask 32" },
    { "Mask33Type1Paint1", 0x1D0, "Panther" },
    { "Mask34Type1Paint1", 0x1D8, "Cap" },
    { "Mask36Type1Paint1", 0x1E0, "Fan" },
    { "Mask37Type1Paint1", 0x1E8, "Mask 37" },
    { "Mask38Type1Paint1", 0x1F0, "Fearsome" },
    { "Mask38Type1Paint2", 0x1F8, "Madman" },
    { "Mask39Type1Paint1", 0x200, "Mask 39 V1" },
    { "Mask39Type1Paint2", 0x208, "Mask 39 V2" },
    { "Mask41Type1Paint1", 0x210, "Silence" },
    { "Mask42Type1Paint1", 0x218, "Mask 42 V1" },
    { "Mask42Type1Paint2", 0x220, "Mask 42 V2" },
    { "Mask43Type1Paint1", 0x228, "Mask 43 V1" },
    { "Mask43Type1Paint2", 0x230, "Mask 43 V2" },
    { "Mask44Type1Paint1", 0x238, "Concrete Gang" },
    { "Mask44Type1Paint2", 0x240, "Gray Gang" },
};

inline const SkinEntry kBagSkins[] = {
    { "Bag1Type1Paint1",   0x8,   "Classic Sporty" },
    { "Bag2Type1Paint1",   0x10,  "Bag 2 V1" },
    { "Bag2Type2Paint1",   0x18,  "Bag 2 V2" },
    { "Bag3Type1Paint1",   0x20,  "Bag 3" },
    { "Bag4Type1Paint1",   0x28,  "Bag 4" },
    { "Bag5Type1Paint1",   0x30,  "Bag 5 V1" },
    { "Bag5Type1Paint2",   0x38,  "Bag 5 V2" },
    { "Bag6Type1Paint1",   0x40,  "Bag 6" },
    { "Bag7Type1Paint1",   0x48,  "Orange Sporty" },
    { "Bag7Type1Paint2",   0x50,  "Tactics Sporty" },
    { "Bag7Type1Paint3",   0x58,  "Edges Sporty" },
    { "Bag8Type1Paint1",   0x60,  "Bag 8" },
    { "Bag9Type1Paint1",   0x68,  "Crocodile Premium" },
    { "Bag10Type1Paint1",  0x70,  "Bag 10 V1" },
    { "Bag10Type2Paint1",  0x78,  "Bag 10 V2" },
    { "Bag11Type1Paint1",  0x80,  "Bag 11" },
    { "Bag12Type1Paint1",  0x88,  "Lady Urban" },
    { "Bag13Type1Paint1",  0x90,  "Red Demon" },
    { "Bag13Type2Paint1",  0x98,  "Brown Demon" },
    { "Bag14Type1Paint1",  0xA0,  "Black Urban" },
    { "Bag14Type1Paint2",  0xA8,  "Urban V2" },
    { "Bag15Type1Paint1",  0xB0,  "DANGER Chemical" },
    { "Bag15Type1Paint2",  0xB8,  "MATRIX Chemical" },
    { "Bag16Type1Paint1",  0xC0,  "Bag 16 V1" },
    { "Bag16Type1Paint2",  0xC8,  "Bag 16 V2" },
    { "Bag17Type1Paint1",  0xD0,  "Bag 17 V1" },
    { "Bag17Type1Paint2",  0xD8,  "Bag 17 V2" },
    { "Bag18Type1Paint1",  0xE0,  "Independence" },
    { "Bag18Type1Paint2",  0xE8,  "Red Hill" },
    { "Bag19Type1Paint1",  0xF0,  "Bag 19" },
    { "Bag20Type1Paint1",  0xF8,  "Bag 20" },
    { "Bag21Type1Paint1",  0x100, "Duffel" },
    { "Bag22Type1Paint1",  0x108, "Bag 22 V1" },
    { "Bag22Type1Paint2",  0x110, "Bag 22 V2" },
    { "Bag23Type1Paint1",  0x118, "Leopard" },
    { "Bag24Type1Paint1",  0x120, "Bag 24" },
    { "Bag25Type1Paint1",  0x128, "Bag 25" },
    { "Bag26Type1Paint1",  0x130, "Klain" },
    { "Bag28Type1Paint1",  0x138, "Bag 28 V1" },
    { "Bag28Type1Paint2",  0x140, "Bag 28 V2" },
    { "Bag28Type1Paint3",  0x148, "Bag 28 V3" },
    { "Bag30Type1Paint1",  0x150, "Bag 30 V1" },
    { "Bag30Type1Paint2",  0x158, "Bag 30 V2" },
    { "Bag31Type1Paint1",  0x160, "Bag 31" },
    { "Bag32Type1Paint1",  0x168, "Extreme" },
};

inline const SkinEntry kKeychainSkins[] = {
    { "NoKeychain",        0x8,   "None" },
    { "Keychain1Paint1",   0x10,  "Keychain 1" },
    { "Keychain2Paint1",   0x18,  "Keychain 2" },
    { "Keychain3Paint1",   0x20,  "Keychain 3" },
    { "Keychain4Paint1",   0x28,  "RIP" },
    { "Keychain6Paint1",   0x30,  "Keychain 6" },
    { "Keychain7Paint1",   0x38,  "Keychain 7" },
    { "Keychain8Paint1",   0x40,  "Keychain 8" },
    { "Keychain9Paint1",   0x48,  "Keychain 9" },
    { "Keychain10Paint1",  0x50,  "Keychain 10" },
    { "Keychain11Paint1",  0x58,  "Keychain 11" },
    { "Keychain12Paint1",  0x60,  "Keychain 12" },
    { "Keychain13Paint1",  0x68,  "Keychain 13" },
    { "Keychain14Paint1",  0x70,  "Keychain 14" },
    { "Keychain15Paint1",  0x78,  "Like" },
    { "Keychain16Paint1",  0x80,  "Peace" },
    { "Keychain17Paint1",  0x88,  "Heart" },
    { "Keychain18Paint1",  0x90,  "Keychain 18" },
    { "Keychain19Paint1",  0x98,  "Alien" },
    { "Keychain20Paint1",  0xA0,  "Keychain 20 V1" },
    { "Keychain20Paint2",  0xA8,  "Keychain 20 V2" },
    { "Keychain21Paint1",  0xB0,  "Keychain 21" },
    { "Keychain22Paint1",  0xB8,  "Keychain 22" },
    { "Keychain23Paint1",  0xC0,  "Keychain 23" },
    { "Keychain24Paint1",  0xC8,  "Keychain 24" },
    { "Keychain25Paint1",  0xD0,  "Candy" },
    { "Keychain26Paint1",  0xD8,  "Keychain 26" },
    { "Keychain27Paint1",  0xE0,  "Keychain 27" },
    { "Keychain28Paint1",  0xE8,  "Keychain 28" },
    { "Keychain29Paint1",  0xF0,  "Brass Knuckles" },
    { "Keychain30Paint1",  0xF8,  "Keychain 30" },
    { "Keychain31Paint1",  0x100, "Keychain 31" },
    { "Keychain33Paint1",  0x108, "Keychain 33" },
    { "Keychain34Paint1",  0x110, "Keychain 34" },
    { "Keychain35Paint1",  0x118, "Keychain 35 V1" },
    { "Keychain35Paint2",  0x120, "Keychain 35 V2" },
    { "Keychain37Paint1",  0x128, "Keychain 37" },
    { "Keychain38Paint1",  0x130, "Keychain 38" },
    { "Keychain39Paint1",  0x138, "Flask" },
    { "Keychain40Paint1",  0x140, "Keychain 40" },
    { "Keychain41Paint1",  0x148, "Keychain 41" },
    { "Keychain42Paint1",  0x150, "Bullet" },
    { "Keychain43Paint1",  0x158, "Keychain 43" },
    { "Keychain44Paint1",  0x160, "Keychain 44" },
    { "Keychain45Paint1",  0x168, "Exhibit" },
    { "Keychain46Paint1",  0x170, "Meat Grinder" },
    { "Keychain47Paint1",  0x178, "Canister" },
    { "Keychain48Paint1",  0x180, "Cellphone" },
    { "Keychain49Paint1",  0x188, "Whistle" },
    { "Keychain51Paint1",  0x190, "Keychain 51" },
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
void ApplySelectedSkins();

} // namespace SkinChanger
